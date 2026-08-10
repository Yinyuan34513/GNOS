/*
 * init.c — GNOS init, PID 1. (GPLv2)
 *
 * This runs in ring 3.  It has no access to I/O ports, no access to kernel
 * memory and no library: everything it does goes through int 0x80.  Its job
 * here is to bring up the terminal (open /dev/tty) and then sit in a tiny
 * read-eval loop, which is the smallest thing that proves the whole stack --
 * loader, VFS, FAT, tty driver, syscalls, privilege switch -- actually works.
 */
#include <stdint.h>

#define SYS_read     0
#define SYS_write    1
#define SYS_open     2
#define SYS_close    3
#define SYS_getpid  39
#define SYS_exit    60

#define O_RDWR       2

static long syscall3(long nr, long a, long b, long c)
{
    long ret;
    asm volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(nr), "D"(a), "S"(b), "d"(c)
                 : "memory", "rcx", "r11");
    return ret;
}

static long sys_open(const char *p, long f) { return syscall3(SYS_open, (long)p, f, 0); }
static long sys_read(long fd, void *b, long n)  { return syscall3(SYS_read, fd, (long)b, n); }
static long sys_write(long fd, const void *b, long n) { return syscall3(SYS_write, fd, (long)b, n); }
static long sys_getpid(void) { return syscall3(SYS_getpid, 0, 0, 0); }
static long sys_close(long fd) { return syscall3(SYS_close, fd, 0, 0); }
static void sys_exit(long c)  { syscall3(SYS_exit, c, 0, 0); for (;;) { } }

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

static void puts_fd(long fd, const char *s)
{
    sys_write(fd, s, (long)slen(s));
}

static void put_dec(long fd, long v)
{
    char buf[24];
    int i = 0;
    if (v == 0)
        buf[i++] = '0';
    while (v > 0) {
        buf[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    char out[24];
    int n = 0;
    while (i--)
        out[n++] = buf[i];
    sys_write(fd, out, n);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++; b++;
    }
    return *a == *b;
}

/* Trim the trailing newline the tty line discipline appends. */
static void chomp(char *s, long n)
{
    if (n > 0 && s[n - 1] == '\n')
        s[n - 1] = 0;
    else
        s[n > 0 ? n : 0] = 0;
}

void _start(void)
{
    long tty = sys_open("/dev/tty", O_RDWR);
    if (tty < 0)
        sys_exit(1);

    puts_fd(tty, "\n");
    puts_fd(tty, "GNOS init: running in ring 3, pid ");
    put_dec(tty, sys_getpid());
    puts_fd(tty, "\n");
    puts_fd(tty, "tty initialised. type 'help' for commands.\n\n");

    char line[128];
    for (;;) {
        puts_fd(tty, "gnos# ");

        long n = sys_read(tty, line, (long)sizeof(line) - 1);
        if (n <= 0)
            continue;
        chomp(line, n);

        if (line[0] == 0)
            continue;
        if (streq(line, "help")) {
            puts_fd(tty, "commands: help, pid, echo <text>, halt\n");
        } else if (streq(line, "pid")) {
            put_dec(tty, sys_getpid());
            puts_fd(tty, "\n");
        } else if (streq(line, "halt")) {
            puts_fd(tty, "init: exiting\n");
            sys_close(tty);
            sys_exit(0);
        } else if (line[0] == 'e' && line[1] == 'c' && line[2] == 'h' &&
                   line[3] == 'o' && (line[4] == ' ' || line[4] == 0)) {
            puts_fd(tty, line[4] ? line + 5 : "");
            puts_fd(tty, "\n");
        } else {
            puts_fd(tty, "init: unknown command: ");
            puts_fd(tty, line);
            puts_fd(tty, "\n");
        }
    }
}
