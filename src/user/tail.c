/*
 * tail.c — print the last few lines of a file or of standard input. (GPLv2)
 *
 * Written as a fixed-size ring of lines so it never needs to know how long
 * the input is and never needs an allocator: line N simply overwrites line
 * N-KEEP, and whatever survives to the end is the answer.
 */
#include "ulib.h"

#define MAX_KEEP  32
#define COLS      160
#define CHUNK     512
#define PATH_MAX  128

static char ring[MAX_KEEP][COLS];
static int  rlen[MAX_KEEP];
static int  head;                     /* next slot to fill */
static int  filled;                   /* how many slots hold a line */
static int  cur;                      /* length of the line being built */

static void reset(void)
{
    head = filled = cur = 0;
}

static void commit(void)
{
    rlen[head] = cur;
    head = (head + 1) % MAX_KEEP;
    if (filled < MAX_KEEP)
        filled++;
    cur = 0;
}

static void feed(char c)
{
    if (c == '\n') {
        commit();
        return;
    }
    if (cur < COLS - 1)
        ring[head][cur++] = c;
}

static void flush(int keep)
{
    if (cur)                          /* a last line with no newline */
        commit();

    int n = (keep < filled) ? keep : filled;
    int i = (head - n + 2 * MAX_KEEP) % MAX_KEEP;

    while (n--) {
        sys_write(1, ring[i], rlen[i]);
        sys_write(1, "\n", 1);
        i = (i + 1) % MAX_KEEP;
    }
}

static int consume(int fd)
{
    char buf[CHUNK];

    for (;;) {
        long n = sys_read(fd, buf, (long)sizeof(buf));
        if (n == 0)
            return 0;
        if (n < 0)
            return 1;
        for (long i = 0; i < n; i++)
            feed(buf[i]);
    }
}

int main(int argc, char **argv)
{
    int keep = 10;
    int i    = 1;

    /* -n COUNT, or the older -COUNT spelling. */
    if (i < argc && argv[i][0] == '-') {
        if (argv[i][1] == 'n' && argv[i][2] == 0 && i + 1 < argc) {
            keep = atoi(argv[i + 1]);
            i += 2;
        } else {
            keep = atoi(argv[i] + 1);
            i++;
        }
        if (keep <= 0)
            keep = 1;
        if (keep > MAX_KEEP)
            keep = MAX_KEEP;
    }

    if (i >= argc) {
        reset();
        int rc = consume(0);
        flush(keep);
        return rc;
    }

    int rc = 0;
    for (; i < argc; i++) {
        char        buf[PATH_MAX];
        const char *path = abspath(argv[i], buf, (int)sizeof(buf));

        int fd = sys_open(path, O_RDONLY);
        if (fd < 0) {
            puts_fd(2, "tail: ");
            puts_fd(2, argv[i]);
            puts_fd(2, ": no such file\n");
            rc = 1;
            continue;
        }

        reset();
        rc |= consume(fd);
        flush(keep);
        sys_close(fd);
    }
    return rc;
}
