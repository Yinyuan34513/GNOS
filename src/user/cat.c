/*
 * cat.c — copy files, or standard input, to standard output. (GPLv2)
 *
 * With no arguments it reads fd 0, which is what makes it useful on the
 * right-hand side of a pipe.
 */
#include "ulib.h"

#define CHUNK    512
#define PATH_MAX 128

static int drain(int fd)
{
    char buf[CHUNK];

    for (;;) {
        long n = sys_read(fd, buf, (long)sizeof(buf));
        if (n == 0)
            return 0;                  /* end of file */
        if (n < 0)
            return 1;                  /* interrupted, or a bad descriptor */

        long done = 0;
        while (done < n) {
            long w = sys_write(1, buf + done, n - done);
            if (w <= 0)
                return 1;
            done += w;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return drain(0);

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char        buf[PATH_MAX];
        const char *path = abspath(argv[i], buf, (int)sizeof(buf));

        gstat_t st;
        if (sys_stat(path, &st) < 0) {
            puts_fd(2, "cat: ");
            puts_fd(2, argv[i]);
            puts_fd(2, ": no such file\n");
            rc = 1;
            continue;
        }
        if (st.kind == GK_DIR) {
            puts_fd(2, "cat: ");
            puts_fd(2, argv[i]);
            puts_fd(2, ": is a directory\n");
            rc = 1;
            continue;
        }

        int fd = sys_open(path, O_RDONLY);
        if (fd < 0) {
            puts_fd(2, "cat: ");
            puts_fd(2, argv[i]);
            puts_fd(2, ": cannot open\n");
            rc = 1;
            continue;
        }
        rc |= drain(fd);
        sys_close(fd);
    }
    return rc;
}
