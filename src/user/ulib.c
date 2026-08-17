/*
 * ulib.c — user-space runtime. (GPLv2)
 *
 * The syscall convention is Linux's: number in RAX, arguments in RDI/RSI/RDX,
 * result in RAX, errors as small negative values.  int 0x80 is the gate; the
 * kernel clobbers nothing else, but RCX and R11 are listed anyway so the
 * compiler never keeps anything live in them across the trap.
 */
#include "ulib.h"

static long syscall3(long nr, long a, long b, long c)
{
    long ret;
    asm volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(nr), "D"(a), "S"(b), "d"(c)
                 : "memory", "rcx", "r11");
    return ret;
}

static long syscall1(long nr, long a)
{
    long ret;
    asm volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(nr), "D"(a)
                 : "memory", "rcx", "r11");
    return ret;
}

/* ---- raw system calls -------------------------------------------------- */
long sys_read(int fd, void *buf, long n)
{
    return syscall3(SYS_read, fd, (long)buf, n);
}

/* dbgputs(441): copy a NUL-terminated string to the debug console. */
long sys_dbgputs(const char *s)
{
    return syscall1(SYS_dbgputs, (long)s);
}

long sys_write(int fd, const void *buf, long n)
{
    return syscall3(SYS_write, fd, (long)buf, n);
}

int sys_open(const char *path, int flags)
{
    return (int)syscall3(SYS_open, (long)path, flags, 0);
}

int sys_close(int fd)          { return (int)syscall3(SYS_close, fd, 0, 0); }
int sys_dup(int fd)            { return (int)syscall3(SYS_dup, fd, 0, 0); }
int sys_dup2(int o, int n)     { return (int)syscall3(SYS_dup2, o, n, 0); }

long sys_lseek(int fd, long off, int whence)
{
    return syscall3(SYS_lseek, fd, off, whence);
}

int sys_stat(const char *path, gstat_t *st)
{
    return (int)syscall3(SYS_gstat, (long)path, (long)st, 0);
}

int mkdir(const char *path)    { return (int)syscall3(SYS_mkdir, (long)path, 0, 0); }
int unlink(const char *path)   { return (int)syscall3(SYS_unlink, (long)path, 0, 0); }
int pipe(int fds[2])           { return (int)syscall3(SYS_pipe, (long)fds, 0, 0); }

int getpid(void)               { return (int)syscall3(SYS_getpid, 0, 0, 0); }
int getppid(void)              { return (int)syscall3(SYS_getppid, 0, 0, 0); }
int fork(void)                 { return (int)syscall3(SYS_fork, 0, 0, 0); }

int execv(const char *path, char *const argv[])
{
    return (int)syscall3(SYS_execve, (long)path, (long)argv, 0);
}

void exit(int status)
{
    syscall3(SYS_exit, status, 0, 0);
    for (;;) { }                       /* the kernel never lets us get here */
}

int waitpid(int pid, int *status, int options)
{
    return (int)syscall3(SYS_wait4, pid, (long)status, options);
}

int kill(int pid, int sig)     { return (int)syscall3(SYS_kill, pid, sig, 0); }
int signal(int sig, int disp)  { return (int)syscall3(SYS_signal, sig, disp, 0); }

int setpgid(int pid, int pgid) { return (int)syscall3(SYS_setpgid, pid, pgid, 0); }
int getpgid(int pid)           { return (int)syscall3(SYS_getpgid, pid, 0, 0); }
void sched_yield(void)         { syscall3(SYS_sched_yield, 0, 0, 0); }

int ioctl(int fd, unsigned long req, void *arg)
{
    return (int)syscall3(SYS_ioctl, fd, (long)req, (long)arg);
}

/*
 * The terminal calls are ordinary ioctls, exactly as they are in a real libc.
 * Note that TIOCGPGRP/TIOCSPGRP take a *pointer* to the group id, not the id
 * itself -- forgetting that is the classic way to hand the terminal to
 * whatever integer happened to be on the stack.
 */
int tcsetpgrp(int fd, int pgid)
{
    return ioctl(fd, TIOCSPGRP, &pgid);
}

int tcgetpgrp(int fd)
{
    int pgid = 0;
    int r = ioctl(fd, TIOCGPGRP, &pgid);
    return r < 0 ? r : pgid;
}

int tcgetattr(int fd, termios_t *t)
{
    return ioctl(fd, TCGETS, t);
}

int tcsetattr(int fd, int action, const termios_t *t)
{
    if (action < TCSANOW || action > TCSAFLUSH)
        return -EINVAL;
    return ioctl(fd, (unsigned long)(TCSETS + action), (void *)t);
}

int isatty(int fd)
{
    winsize_t ws;
    return ioctl(fd, TIOCGWINSZ, &ws) == 0;
}

/* ---- strings and memory ------------------------------------------------ */
size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)((unsigned char)*a - (unsigned char)*b);
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)((unsigned char)a[i] - (unsigned char)b[i]);
        if (!a[i])
            break;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c)
            return (char *)s;
    return (*s == (char)c) ? (char *)s : 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

int atoi(const char *s)
{
    int sign = 1, v = 0;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return sign * v;
}

const char *abspath(const char *name, char *buf, int cap)
{
    if (!name || name[0] == '/')
        return name;

    int n = 0;
    buf[n++] = '/';
    while (*name && n < cap - 1)
        buf[n++] = *name++;
    buf[n] = 0;
    return buf;
}

/* ---- output ------------------------------------------------------------ */
void puts_fd(int fd, const char *s)
{
    sys_write(fd, s, (long)strlen(s));
}

void putn_fd(int fd, long v)
{
    char tmp[24], out[24];
    int i = 0, n = 0;

    if (v < 0) {
        out[n++] = '-';
        v = -v;
    }
    if (v == 0)
        tmp[i++] = '0';
    while (v > 0) {
        tmp[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (i--)
        out[n++] = tmp[i];

    sys_write(fd, out, n);
}

void print(const char *s)  { puts_fd(1, s); }
void printn(long v)        { putn_fd(1, v); }
