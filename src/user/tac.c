/*
 * tac.c — print lines in reverse order. (GPLv2)
 *
 * Unlike tail, tac cannot stream: the first line it prints is the last one
 * it reads, so the whole input has to be held.  The buffer is a fixed 16 KiB
 * and input past that is dropped rather than silently reordered.
 */
#include "ulib.h"

#define BUF_CAP   16384
#define CHUNK     512
#define PATH_MAX  128

static char buf[BUF_CAP];
static int  len;

static int slurp(int fd, const char *what)
{
    len = 0;

    for (;;) {
        if (len >= BUF_CAP) {
            puts_fd(2, "tac: ");
            puts_fd(2, what);
            puts_fd(2, ": input too large, truncated\n");
            return 1;
        }

        long room = BUF_CAP - len;
        if (room > CHUNK)
            room = CHUNK;

        long n = sys_read(fd, buf + len, room);
        if (n == 0)
            return 0;
        if (n < 0)
            return 1;
        len += (int)n;
    }
}

static void emit(void)
{
    int end = len;
    if (end > 0 && buf[end - 1] == '\n')
        end--;                        /* the terminator of the last line */

    while (end > 0) {
        int start = end;
        while (start > 0 && buf[start - 1] != '\n')
            start--;

        sys_write(1, buf + start, end - start);
        sys_write(1, "\n", 1);
        end = start - 1;              /* step over the '\n' we stopped at */
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        int rc = slurp(0, "stdin");
        emit();
        return rc;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char        pb[PATH_MAX];
        const char *path = abspath(argv[i], pb, (int)sizeof(pb));

        int fd = sys_open(path, O_RDONLY);
        if (fd < 0) {
            puts_fd(2, "tac: ");
            puts_fd(2, argv[i]);
            puts_fd(2, ": no such file\n");
            rc = 1;
            continue;
        }

        rc |= slurp(fd, argv[i]);
        emit();
        sys_close(fd);
    }
    return rc;
}
