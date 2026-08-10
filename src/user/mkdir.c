/*
 * mkdir.c — create directories. (GPLv2)
 */
#include "ulib.h"

#define PATH_MAX 128

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts_fd(2, "usage: mkdir <name>...\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char        buf[PATH_MAX];
        const char *path = abspath(argv[i], buf, (int)sizeof(buf));

        int r = mkdir(path);
        if (r == 0)
            continue;

        puts_fd(2, "mkdir: ");
        puts_fd(2, argv[i]);
        if (r == -EEXIST)
            puts_fd(2, ": already exists\n");
        else if (r == -ENOENT)
            puts_fd(2, ": no such parent directory\n");
        else if (r == -EINVAL)
            puts_fd(2, ": not a valid 8.3 name\n");
        else if (r == -ENOSPC)
            puts_fd(2, ": no space left\n");
        else
            puts_fd(2, ": cannot create\n");
        rc = 1;
    }
    return rc;
}
