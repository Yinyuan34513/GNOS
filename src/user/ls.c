/*
 * ls.c — list directory contents. (GPLv2)
 *
 * Also installed as "dir".  A directory descriptor does not yield bytes, it
 * yields a whole number of gdirent_t records, so listing a directory is a
 * plain read() into an array with no parsing and no allocation.
 */
#include "ulib.h"

#define BATCH 16
#define PATH_MAX 128

static void column(const char *s, int width)
{
    print(s);
    int pad = width - (int)strlen(s);
    while (pad-- > 0)
        print(" ");
}

static void show(const gdirent_t *e)
{
    column(e->name, 16);
    if (e->kind == GK_DIR)
        print("<DIR>");
    else
        printn((long)e->size);
    print("\n");
}

static int list_dir(const char *path)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        puts_fd(2, "ls: cannot open ");
        puts_fd(2, path);
        puts_fd(2, "\n");
        return 1;
    }

    gdirent_t batch[BATCH];
    for (;;) {
        long n = sys_read(fd, batch, (long)sizeof(batch));
        if (n <= 0)
            break;

        int count = (int)(n / (long)sizeof(gdirent_t));
        for (int i = 0; i < count; i++)
            show(&batch[i]);
    }

    sys_close(fd);
    return 0;
}

static int list(const char *name, int show_header, const char *argv0)
{
    char        buf[PATH_MAX];
    const char *path = abspath(name, buf, (int)sizeof(buf));

    gstat_t st;
    if (sys_stat(path, &st) < 0) {
        puts_fd(2, argv0);
        puts_fd(2, ": ");
        puts_fd(2, name);
        puts_fd(2, ": no such file or directory\n");
        return 1;
    }

    if (st.kind != GK_DIR) {
        column(name, 16);
        printn((long)st.size);
        print("\n");
        return 0;
    }

    if (show_header) {
        print(path);
        print(":\n");
    }
    return list_dir(path);
}

int main(int argc, char **argv)
{
    const char *me = (argc > 0 && argv[0]) ? argv[0] : "ls";

    if (argc < 2)
        return list("/", 0, me);

    int rc = 0;
    for (int i = 1; i < argc; i++)
        rc |= list(argv[i], argc > 2, me);
    return rc;
}
