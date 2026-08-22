/*
 * shell.c — GNOS shell: job control, pipelines and redirection. (GPLv2)
 *
 * Started by init as a child process, and owning the terminal from the
 * moment init hands it over with tcsetpgrp().
 *
 * The job-control rules it implements are the POSIX ones, and each of them
 * exists for a reason worth spelling out:
 *
 *   - Every *pipeline* runs in a process group of its own, created by the
 *     first child with setpgid(0, 0), joined by the rest, and set from the
 *     parent side as well because whichever runs first must win the race.
 *     One group per pipeline is what makes a single Ctrl-C stop all of it.
 *
 *   - The foreground job is whichever group currently owns the terminal.
 *     tcsetpgrp() is the whole of "foreground": the kernel sends keyboard
 *     signals to that group and stops anybody else who tries to read.
 *
 *   - The shell ignores SIGINT/SIGQUIT/SIGTSTP/SIGTTOU.  It has to: it lives
 *     in the same session as the jobs it starts, and without this a Ctrl-C
 *     aimed at a job would kill the shell as well.  Children put the
 *     dispositions back before exec, which is what makes Ctrl-C work on
 *     them.
 *
 *   - waitpid(..., WUNTRACED) is how the shell learns that a job stopped
 *     rather than finished, which is the difference between printing
 *     "Stopped" and forgetting about it.
 *
 * A command that names no existing program is reported before anything is
 * forked.  There is no search path to walk -- the initrd is one flat volume
 * -- so "does /<name>.elf exist?" is a single stat(), and answering it up
 * front is both cheaper and clearer than forking a process only to have exec
 * fail inside it.
 */
#include "ulib.h"

#define MAX_LINE   256
#define MAX_ARGS   16
#define MAX_JOBS   8
#define MAX_STAGES 4
#define CMD_LEN    64
#define PATH_LEN   96

typedef struct {
    int  used;
    int  pgid;
    int  npid;
    int  pid[MAX_STAGES];       /* zeroed as each member is reaped */
    int  stopped;
    char cmd[CMD_LEN];
} job_t;

/* One command in a pipeline, after redirections have been pulled out. */
typedef struct {
    char *av[MAX_ARGS];
    char *in_file;
    char *out_file;
    int   append;
    char  path[PATH_LEN];       /* the resolved program, for non-builtins */
} stage_t;

static job_t   jobs[MAX_JOBS];
static stage_t stages[MAX_STAGES];
static int     shell_pgid;

/*
 * The environment.  rc drives the whole desktop through this shell, and the
 * compositor/session need variables it sets (XKB_CONFIG_ROOT,
 * DBUS_SESSION_BUS_ADDRESS, WAYLAND_DISPLAY, ...).  `export NAME=value`
 * stores here and every execve hands the table down; without it the shell's
 * execv ran every child with an empty environment and libxkbcommon fell back
 * to its compiled-in host path.
 */
#define MAX_ENV 64
#define ENV_LEN 256
static char   env_tab[MAX_ENV][ENV_LEN];
static int    env_n;
static char  *env_p[MAX_ENV + 1];

static void env_set(const char *kv)
{
    const char *eq = strchr(kv, '=');
    if (!eq || eq == kv || !*eq)
        return;
    size_t len = strlen(kv);
    if (len >= ENV_LEN)
        return;
    for (int i = 0; i < env_n; i++)
        if (strncmp(env_tab[i], kv, (size_t)(eq - kv)) == 0 &&
            env_tab[i][eq - kv] == '=') {
            strcpy(env_tab[i], kv);
            return;
        }
    if (env_n < MAX_ENV)
        strcpy(env_tab[env_n++], kv);
}

static char *const *env_all(void)
{
    for (int i = 0; i < env_n; i++)
        env_p[i] = env_tab[i];
    env_p[env_n] = 0;
    return env_p;
}


/*
 * A private descriptor for the terminal, duplicated from stdin at startup.
 * Script mode splices the script onto fd 0, and a redirected fd 0 is not the
 * tty any more -- so every tcsetpgrp() has to go through this one instead, or
 * handing the terminal to a job would quietly fail with ENOTTY.
 */
static int     shell_tty;

static int run_builtin(char **av);
static int is_builtin(const char *name);

/* ---- jobs ------------------------------------------------------------- */
static int job_index(job_t *j) { return (int)(j - jobs); }

static job_t *job_add(int *pids, int n, int pgid, const char *cmd)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].used)
            continue;

        jobs[i].used    = 1;
        jobs[i].pgid    = pgid;
        jobs[i].npid    = n;
        jobs[i].stopped = 0;
        for (int k = 0; k < n; k++)
            jobs[i].pid[k] = pids[k];

        int c = 0;
        while (cmd[c] && c < CMD_LEN - 1) {
            jobs[i].cmd[c] = cmd[c];
            c++;
        }
        jobs[i].cmd[c] = 0;
        return &jobs[i];
    }
    return 0;
}

static job_t *job_by_pid(int pid)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used)
            continue;
        for (int k = 0; k < jobs[i].npid; k++)
            if (jobs[i].pid[k] == pid)
                return &jobs[i];
    }
    return 0;
}

static void job_banner(job_t *j, const char *what)
{
    print("[");
    printn(job_index(j) + 1);
    print("] ");
    print(what);
    print("\t");
    print(j->cmd);
    print("\n");
}

/* Fold one wait() result into the job it belongs to.  A job is only "Done"
 * once every member of the pipeline has gone. */
static void job_update(job_t *j, int pid, int status)
{
    if (WIFSTOPPED(status)) {
        if (!j->stopped) {
            j->stopped = 1;
            job_banner(j, "Stopped");
        }
        return;
    }

    for (int i = 0; i < j->npid; i++)
        if (j->pid[i] == pid)
            j->pid[i] = 0;

    for (int i = 0; i < j->npid; i++)
        if (j->pid[i])
            return;

    job_banner(j, WIFSIGNALED(status) ? "Terminated" : "Done");
    j->used = 0;
}

/* Pick up whatever happened to background jobs while we were not looking. */
static void reap_jobs(void)
{
    for (;;) {
        int status = 0;
        int pid = waitpid(-1, &status, WNOHANG | WUNTRACED);
        if (pid <= 0)
            break;

        job_t *j = job_by_pid(pid);
        if (j)
            job_update(j, pid, status);
    }
}

/* ---- foreground handling ---------------------------------------------- */
/*
 * Give `pgid` the terminal, wait for every member of the pipeline to finish
 * or stop, then take the terminal back.  Everything that makes a job "the
 * foreground job" is in these few lines.
 */
static void wait_foreground(int *pids, int n, int pgid, const char *cmd)
{
    tcsetpgrp(shell_tty, pgid);

    int stopped = 0;
    int termsig = 0;

    for (int i = 0; i < n; i++) {
        int status = 0;
        int who = waitpid(pids[i], &status, WUNTRACED);
        if (who < 0)
            continue;
        if (WIFSTOPPED(status))
            stopped = 1;
        else if (WIFSIGNALED(status))
            termsig = WTERMSIG(status);
    }

    tcsetpgrp(shell_tty, shell_pgid);

    if (stopped) {
        job_t *j = job_add(pids, n, pgid, cmd);
        if (j) {
            j->stopped = 1;
            job_banner(j, "Stopped");
        }
    } else if (termsig) {
        print("Terminated by signal ");
        printn(termsig);
        print("\n");
    }
}

/* ---- resolving a command name ----------------------------------------- */
/*
 * A bare command name is looked up along PATH, trying both "<name>" and
 * "<name>.elf" in each directory.  Absolute paths ("/bin/ls") and paths
 * with a slash ("bin/ls") are taken relative to the root.  The first
 * candidate that stat()s as a regular file wins; if none does we fall back
 * to the old "/<name>.elf" so the "no such program" message still reads
 * sensibly.
 */
static const char *g_path[] = { "/bin", "/usr/bin", "/sbin",
                                "/usr/sbin", "" };

static int program_exists(const char *path);   /* defined just below */

static void resolve(const char *name, char *buf, int cap)
{
    if (name[0] == '/') {
        int n = 0;
        while (name[n] && n < cap - 1) {
            buf[n] = name[n];
            n++;
        }
        buf[n] = 0;
        return;
    }

    /* A relative path with a slash ("bin/ls") is resolved against root. */
    if (strchr(name, '/')) {
        int n = 0;
        buf[n++] = '/';
        for (const char *p = name; *p && n < cap - 1; p++)
            buf[n++] = *p;
        buf[n] = 0;
        return;
    }

    int has_dot = 0;
    for (const char *p = name; *p; p++)
        if (*p == '.')
            has_dot = 1;

    char base[PATH_LEN];
    int bn = 0;
    for (const char *p = name; *p && bn < (int)sizeof(base) - 6; p++)
        base[bn++] = *p;
    base[bn] = 0;

    for (int d = 0; d < (int)(sizeof(g_path) / sizeof(g_path[0])); d++) {
        const char *dir = g_path[d];
        int n = 0;
        if (dir[0]) {
            for (const char *p = dir; *p && n < cap - 1; p++)
                buf[n++] = *p;
            buf[n++] = '/';
        } else {
            buf[n++] = '/';
        }
        for (int i = 0; i < bn && n < cap - 1; i++)
            buf[n++] = base[i];
        if (!has_dot) {
            buf[n++] = '.';
            buf[n++] = 'e';
            buf[n++] = 'l';
            buf[n++] = 'f';
        }
        buf[n] = 0;
        if (program_exists(buf))
            return;
    }

    /* Not found on PATH: leave /<name>.elf for the error path. */
    int n = 0;
    buf[n++] = '/';
    for (int i = 0; i < bn && n < cap - 1; i++)
        buf[n++] = base[i];
    if (!has_dot) {
        buf[n++] = '.';
        buf[n++] = 'e';
        buf[n++] = 'l';
        buf[n++] = 'f';
    }
    buf[n] = 0;
}

static int program_exists(const char *path)
{
    gstat_t st;
    if (sys_stat(path, &st) < 0)
        return 0;
    return st.kind == GK_FILE;
}

/* ---- redirection ------------------------------------------------------ */
static int redirect(const char *name, int flags, int target, const char *what)
{
    char        buf[PATH_LEN];
    const char *path = abspath(name, buf, (int)sizeof(buf));

    int fd = sys_open(path, flags);
    if (fd < 0) {
        puts_fd(2, "shell: ");
        puts_fd(2, name);
        puts_fd(2, what);
        return 0;
    }
    sys_dup2(fd, target);
    sys_close(fd);
    return 1;
}

static int apply_redirects(stage_t *s)
{
    if (s->in_file &&
        !redirect(s->in_file, O_RDONLY, 0, ": cannot open for reading\n"))
        return 0;

    if (s->out_file &&
        !redirect(s->out_file, O_WRONLY | O_CREAT |
                  (s->append ? O_APPEND : O_TRUNC), 1,
                  ": cannot open for writing\n"))
        return 0;

    return 1;
}

/* ---- running a pipeline ----------------------------------------------- */
static void child_setup(int pgid, int background)
{
    setpgid(0, pgid);                 /* pgid == 0: become the leader */
    if (!background && pgid == 0)
        tcsetpgrp(shell_tty, getpid());

    /* The tcsetpgrp() above has to happen while SIGTTOU is still inherited
     * as SIG_IGN: we are already in our own (background) group by then, so
     * a terminal operation from here would otherwise stop us before we ever
     * reach exec.  Only after it is done do the defaults go back.
     *
     * Inherited "ignore" dispositions would also make the job immune to
     * Ctrl-C, which is precisely what we do not want. */
    signal(SIGINT,  SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
}

static void run_pipeline(int n, int background, const char *cmd)
{
    /* A lone builtin runs in the shell itself: `cd`-like commands would be
     * pointless in a child, and `exit` has to be. */
    if (n == 1 && !stages[0].in_file && !stages[0].out_file &&
        is_builtin(stages[0].av[0])) {
        run_builtin(stages[0].av);
        return;
    }

    /* Check every program before forking anything: a pipeline that cannot
     * run should not leave half of itself running. */
    for (int i = 0; i < n; i++) {
        if (is_builtin(stages[i].av[0]))
            continue;

        resolve(stages[i].av[0], stages[i].path, PATH_LEN);
        if (!program_exists(stages[i].path)) {
            print("shell: ");
            print(stages[i].av[0]);
            print(": no such program\n");
            return;
        }
    }

    int pids[MAX_STAGES];
    int count = 0;
    int pgid  = 0;
    int in    = -1;

    for (int i = 0; i < n; i++) {
        int pfd[2] = { -1, -1 };

        if (i + 1 < n && pipe(pfd) < 0) {
            print("shell: cannot create pipe\n");
            break;
        }

        int pid = fork();
        if (pid < 0) {
            print("shell: fork failed\n");
            if (pfd[0] >= 0) {
                sys_close(pfd[0]);
                sys_close(pfd[1]);
            }
            break;
        }

        if (pid == 0) {
            child_setup(pgid, background);

            if (in >= 0) {
                sys_dup2(in, 0);
                sys_close(in);
            }
            if (pfd[1] >= 0) {
                sys_dup2(pfd[1], 1);
                sys_close(pfd[1]);
            }
            if (pfd[0] >= 0)
                sys_close(pfd[0]);

            if (!apply_redirects(&stages[i]))
                exit(1);

            if (is_builtin(stages[i].av[0])) {
                run_builtin(stages[i].av);
                exit(0);
            }

            execve(stages[i].path, stages[i].av, env_all());
            puts_fd(2, "shell: exec failed\n");
            exit(126);
        }

        if (pgid == 0)
            pgid = pid;
        setpgid(pid, pgid);
        pids[count++] = pid;

        /* The shell must not keep a pipe end open: the reader only sees end
         * of file once *every* copy of the write end is gone. */
        if (in >= 0)
            sys_close(in);
        if (pfd[1] >= 0)
            sys_close(pfd[1]);
        in = pfd[0];
    }

    if (in >= 0)
        sys_close(in);
    if (!count)
        return;

    if (background) {
        job_t *j = job_add(pids, count, pgid, cmd);
        if (j) {
            print("[");
            printn(job_index(j) + 1);
            print("] ");
            printn(pids[count - 1]);
            print("\n");
        }
    } else {
        wait_foreground(pids, count, pgid, cmd);
    }
}

/* ---- builtins --------------------------------------------------------- */
static job_t *job_by_spec(const char *spec)
{
    if (spec && *spec) {
        int n = atoi(spec);
        if (n >= 1 && n <= MAX_JOBS && jobs[n - 1].used)
            return &jobs[n - 1];
        return 0;
    }

    /* No argument: the most recently stopped job, else the last one. */
    for (int i = MAX_JOBS - 1; i >= 0; i--)
        if (jobs[i].used && jobs[i].stopped)
            return &jobs[i];
    for (int i = MAX_JOBS - 1; i >= 0; i--)
        if (jobs[i].used)
            return &jobs[i];
    return 0;
}

static job_t *pick_job(char **av)
{
    job_t *j = job_by_spec(av[1]);
    if (!j)
        print(av[1] ? "shell: no such job\n" : "shell: no current job\n");
    return j;
}

static void builtin_jobs(void)
{
    int any = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used)
            continue;
        any = 1;
        print("[");
        printn(i + 1);
        print("] ");
        printn(jobs[i].pgid);
        print("\t");
        print(jobs[i].stopped ? "Stopped" : "Running");
        print("\t");
        print(jobs[i].cmd);
        print("\n");
    }
    if (!any)
        print("shell: no jobs\n");
}

static void builtin_fg(char **av)
{
    job_t *j = pick_job(av);
    if (!j)
        return;

    print(j->cmd);
    print("\n");

    kill(-j->pgid, SIGCONT);
    j->stopped = 0;

    int pids[MAX_STAGES], n = 0;
    for (int i = 0; i < j->npid; i++)
        if (j->pid[i])
            pids[n++] = j->pid[i];

    int  pgid = j->pgid;
    char cmd[CMD_LEN];
    strcpy(cmd, j->cmd);
    j->used = 0;                    /* it is the foreground job now */

    wait_foreground(pids, n, pgid, cmd);
}

static void builtin_bg(char **av)
{
    job_t *j = pick_job(av);
    if (!j)
        return;
    if (!j->stopped) {
        print("shell: job is already running\n");
        return;
    }

    kill(-j->pgid, SIGCONT);
    j->stopped = 0;
    job_banner(j, "Running");
}

/*
 * kill [-SIGNUM] pid|%job ...
 *
 * "%n" names a job rather than a process, and signals the whole group -- a
 * pipeline is several processes and killing only the first would leave the
 * rest of it running.
 */
static void builtin_kill(char **av)
{
    int sig = SIGTERM;
    int i   = 1;

    if (av[i] && av[i][0] == '-' && av[i][1]) {
        sig = atoi(av[i] + 1);
        if (sig <= 0 || sig >= 32) {
            print("shell: kill: bad signal number\n");
            return;
        }
        i++;
    }

    if (!av[i]) {
        print("usage: kill [-signum] pid | %job ...\n");
        return;
    }

    for (; av[i]; i++) {
        int target;

        if (av[i][0] == '%') {
            job_t *j = job_by_spec(av[i] + 1);
            if (!j) {
                print("shell: kill: no such job\n");
                continue;
            }
            target = -j->pgid;
            /* A stopped job cannot act on a signal until it runs again. */
            if (j->stopped && sig != SIGKILL)
                kill(target, SIGCONT);
        } else {
            target = atoi(av[i]);
            if (target == 0) {
                print("shell: kill: ");
                print(av[i]);
                print(": not a pid\n");
                continue;
            }
        }

        if (kill(target, sig) < 0) {
            print("shell: kill: ");
            print(av[i]);
            print(": no such process\n");
        }
    }
}

static void builtin_help(void)
{
    print("builtins:\n");
    print("  help              this text\n");
    print("  echo <text>       print text\n");
    print("  jobs              list background and stopped jobs\n");
    print("  fg [n] / bg [n]   resume job n in the fore/background\n");
    print("  kill [-sig] p|%n  signal a process or a whole job\n");
    print("  pid               print the shell's pid and pgid\n");
    print("  exit [n]          leave the shell (init restarts it)\n");
    print("programs: searched via PATH in /bin /usr/bin /sbin /usr/sbin\n");
    print("(ls dir cat tail tac rm mkdir touch count live in /bin)\n");
    print("syntax: cmd args, a | b | c, < in, > out, >> append, & background\n");
    print("keys: ^C interrupt, ^Z suspend, ^D end of input\n");
}

static int is_builtin(const char *name)
{
    static const char *names[] = { "help", "echo", "jobs", "fg", "bg",
                                   "kill", "pid", "exit", "export", 0 };
    for (int i = 0; names[i]; i++)
        if (strcmp(name, names[i]) == 0)
            return 1;
    return 0;
}

/* Returns 1 if the line was a builtin. */
static int run_builtin(char **av)
{
    if (strcmp(av[0], "export") == 0) {
        for (int i = 1; av[i]; i++)
            env_set(av[i]);
        return 1;
    }
    if (strcmp(av[0], "help") == 0) {
        builtin_help();
        return 1;
    }
    if (strcmp(av[0], "jobs") == 0) {
        builtin_jobs();
        return 1;
    }
    if (strcmp(av[0], "fg") == 0) {
        builtin_fg(av);
        return 1;
    }
    if (strcmp(av[0], "bg") == 0) {
        builtin_bg(av);
        return 1;
    }
    if (strcmp(av[0], "kill") == 0) {
        builtin_kill(av);
        return 1;
    }
    if (strcmp(av[0], "pid") == 0) {
        print("pid ");
        printn(getpid());
        print(", pgid ");
        printn(getpgid(0));
        print(", ppid ");
        printn(getppid());
        print("\n");
        return 1;
    }
    if (strcmp(av[0], "echo") == 0) {
        for (int i = 1; av[i]; i++) {
            if (i > 1)
                print(" ");
            print(av[i]);
        }
        print("\n");
        return 1;
    }
    if (strcmp(av[0], "exit") == 0)
        exit(av[1] ? atoi(av[1]) : 0);

    return 0;
}

/* ---- parsing ---------------------------------------------------------- */
static int split(char *text, char **av)
{
    int   n = 0;
    char *p = text;

    while (*p && n < MAX_ARGS - 1) {
        while (*p == ' ' || *p == '\t')
            *p++ = 0;
        if (!*p)
            break;
        av[n++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    av[n] = 0;
    return n;
}

/*
 * Pull "< file", "> file" and ">> file" out of an argument list.  The
 * operand may be attached ("<in") or separate ("< in"), which is the only
 * bit of shell syntax that needs looking at twice.
 */
static int extract_redirects(stage_t *s)
{
    char **av = s->av;
    int    w  = 0;

    for (int r = 0; av[r]; r++) {
        char c = av[r][0];
        if (c != '<' && c != '>') {
            av[w++] = av[r];
            continue;
        }

        int append = (c == '>' && av[r][1] == '>');
        char *rest = av[r] + (append ? 2 : 1);

        if (!*rest) {
            if (!av[r + 1]) {
                print("shell: missing name after redirection\n");
                return 0;
            }
            rest = av[++r];
        }

        if (c == '<') {
            s->in_file = rest;
        } else {
            s->out_file = rest;
            s->append   = append;
        }
    }

    av[w] = 0;
    if (w == 0) {
        print("shell: missing command\n");
        return 0;
    }
    return 1;
}

/* Cut the line at every '|' and parse each piece.  Returns the number of
 * stages, or 0 if the line does not make sense. */
static int parse_line(char *line)
{
    char *piece[MAX_STAGES];
    int   n = 0;

    piece[n++] = line;
    for (char *p = line; *p; p++) {
        if (*p != '|')
            continue;
        *p = 0;
        if (n >= MAX_STAGES) {
            print("shell: pipeline too long\n");
            return 0;
        }
        piece[n++] = p + 1;
    }

    for (int i = 0; i < n; i++) {
        stages[i].in_file  = 0;
        stages[i].out_file = 0;
        stages[i].append   = 0;

        if (split(piece[i], stages[i].av) == 0) {
            print("shell: empty command in pipeline\n");
            return 0;
        }
        if (!extract_redirects(&stages[i]))
            return 0;
    }
    return n;
}

/* ---- the loop --------------------------------------------------------- */
/*
 * Read one line, no matter how the input arrives.  The terminal yields one
 * line per read, but a script file returns a whole block at once, so we keep
 * a buffer and hand back a single newline-terminated line each call, saving
 * the remainder for next time.  Returns the line length (without the '\n'),
 * or -1 at end of file.
 */
static char  g_rdbuf[1024];
static int   g_rdlo;          /* first unread byte */
static int   g_rdlen;         /* bytes held in g_rdbuf */

static long read_line(char *line, int cap)
{
    int n = 0;
    for (;;) {
        if (g_rdlo >= g_rdlen) {
            g_rdlen = (int)sys_read(0, g_rdbuf, (long)sizeof(g_rdbuf));
            g_rdlo  = 0;
            if (g_rdlen <= 0)
                return n ? n : -1;     /* flush a final partial line, else EOF */
        }
        char c = g_rdbuf[g_rdlo++];
        if (c == '\n')
            return n;
        if (n < cap - 1)
            line[n++] = c;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* We were put in our own group by init; remember it, because every
     * foreground job we start has to hand the terminal back to it. */
    shell_pgid = getpgid(0);

    /* Grab the terminal before anything can redirect fd 0 out from under us
     * (script mode does exactly that a few lines down). */
    shell_tty = sys_dup(0);
    if (shell_tty < 0)
        shell_tty = 0;

    signal(SIGINT,  SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    /* Script mode: a path argument means "read commands from here instead
     * of the terminal".  Splice the file onto stdin; the loop below is
     * unchanged and end-of-file ends the script exactly like Ctrl-D.  A
     * missing script is non-fatal so a absent /etc/rc never stops boot. */
    if (argc > 1) {
        int fd = sys_open(argv[1], O_RDONLY);
        if (fd < 0) {
            puts_fd(2, "shell: cannot open script ");
            puts_fd(2, argv[1]);
            puts_fd(2, "\n");
            return 1;
        }
        sys_dup2(fd, 0);
        sys_close(fd);
    } else {
        print("GNOS shell, pid ");
        printn(getpid());
        print(" pgid ");
        printn(shell_pgid);
        print(". type 'help'.\n");
    }

    char line[MAX_LINE];
    char raw[CMD_LEN];

    /* Script mode (argc > 1) reads from a file and stays silent; the prompt
     * and job banners would just be noise in the boot log. */
    int interactive = (argc <= 1);

    for (;;) {
        reap_jobs();

        if (interactive)
            print("gnos$ ");

        long n = read_line(line, (int)sizeof(line));
        if (n < 0) {                        /* Ctrl-D / end of script */
            if (interactive)
                print("\n");
            exit(0);
        }
        line[n] = 0;

        /* Comments start with '#'; skip the whole line. */
        int c = 0;
        while (c < n && (line[c] == ' ' || line[c] == '\t'))
            c++;
        if (line[c] == '#')
            continue;

        /* Keep a pristine copy for the jobs list before parsing chops it. */
        int i = 0;
        while (i < CMD_LEN - 1 && line[i]) {
            raw[i] = line[i];
            i++;
        }
        raw[i] = 0;

        int background = 0;
        while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t'))
            line[--n] = 0;
        if (n > 0 && line[n - 1] == '&') {
            background = 1;
            line[--n] = 0;
        }

        int blank = 1;
        for (const char *q = line; *q; q++) {
            if (*q != ' ' && *q != '\t') {
                blank = 0;
                break;
            }
        }
        if (blank)
            continue;

        int stage_count = parse_line(line);
        if (stage_count > 0)
            run_pipeline(stage_count, background, raw);
    }
}
