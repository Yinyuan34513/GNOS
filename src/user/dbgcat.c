/*
 * dbgcat.c — copy files to the debug console (GPLv2, ulib).
 *
 * The headless-test counterpart of cat(1): it reads each file named on the
 * command line and pushes its contents to the isa-debugcon port through the
 * dbgputs(441) syscall, so `make test`'s captured build/dbg.log shows the
 * test output that normally scrolls across the framebuffer where nobody
 * headless can see it.  Used from /etc/rc to report the boot-time tests.
 *
 * The syscall is NUL-terminated-string shaped, so the file is read whole
 * into a buffer first and pushed in NUL-delimited pieces.
 */
#include "ulib.h"

static long dbgputs(char *s, long n)
{
    /* dbgputs takes a NUL-terminated string; hand it one line at a time.
     * The NULs are written into our own buffer, so s must be mutable. */
    long off = 0;
    while (off < n) {
        long end = off;
        while (end < n && s[end] != '\n' && s[end] != 0)
            end++;
        if (end < n && s[end] == 0)
            break;                      /* stray NUL: stop, not worth chasing */
        s[end] = 0;                     /* in our buffer: safe */
        long r = sys_dbgputs(&s[off]);
        if (r < 0)
            return r;
        off = end + 1;
    }
    return 0;
}

#define CHUNK 2048

int main(int argc, char **argv)
{
    int rc = 0;
    char buf[CHUNK + 1];

    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], 0);
        if (fd < 0) {
            print("dbgcat: cannot open ");
            print(argv[i]);
            print("\n");
            rc = 1;
            continue;
        }
        for (;;) {
            long n = sys_read(fd, buf, CHUNK);
            if (n <= 0)
                break;
            buf[n] = 0;
            if (dbgputs(buf, n) < 0) {
                rc = 1;
                break;
            }
        }
        sys_close(fd);
    }
    return rc;
}
