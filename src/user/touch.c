/*
 * touch.c — create empty files. (GPLv2)
 *
 * There are no timestamps on this volume yet, so touching a file that already
 * exists is a no-op rather than an error -- the useful half of touch is
 * O_CREAT.
 */
#include "ulib.h"

#define PATH_MAX 128

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts_fd(2, "usage: touch <name>...\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char        buf[PATH_MAX];
        const char *path = abspath(argv[i], buf, (int)sizeof(buf));

        int fd = sys_open(path, O_WRONLY | O_CREAT);
        if (fd < 0) {
            puts_fd(2, "touch: ");
            puts_fd(2, argv[i]);
            if (fd == -EINVAL)
                puts_fd(2, ": not a valid 8.3 name\n");
            else if (fd == -ENOSPC)
                puts_fd(2, ": no space left\n");
            else
                puts_fd(2, ": cannot create\n");
            rc = 1;
            continue;
        }
        sys_close(fd);
    }
    return rc;
}
