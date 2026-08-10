/*
 * rm.c — remove files and empty directories. (GPLv2)
 */
#include "ulib.h"

#define PATH_MAX 128

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts_fd(2, "usage: rm <name>...\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char        buf[PATH_MAX];
        const char *path = abspath(argv[i], buf, (int)sizeof(buf));

        int r = unlink(path);
        if (r == 0)
            continue;

        puts_fd(2, "rm: ");
        puts_fd(2, argv[i]);
        if (r == -ENOENT)
            puts_fd(2, ": no such file\n");
        else if (r == -ENOTEMPTY)
            puts_fd(2, ": directory not empty\n");
        else if (r == -EPERM)
            puts_fd(2, ": operation not permitted\n");
        else
            puts_fd(2, ": cannot remove\n");
        rc = 1;
    }
    return rc;
}
