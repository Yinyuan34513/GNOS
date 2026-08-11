/*
 * coldplug.c — the device manager, in the mdev/udev-trigger sense. (GPLv2)
 *
 * "Coldplug" is the pass that runs once at boot and reconciles /dev with the
 * devices the kernel already found, as opposed to the hotplug path that
 * reacts to devices appearing later.  On Linux that means walking /sys and
 * calling mknod; here the kernel's /dev is synthetic -- every registered
 * character device is already resolvable by name -- so the reconciliation is
 * a *verification* rather than a creation:
 *
 *   for every subsystem the kernel says is live and publishes a node,
 *   that node must be openable, and it must behave.
 *
 * That is worth doing rather than assuming, because the two halves are
 * maintained separately: a driver can register itself in the subsystem
 * registry and forget to call vfs_register_dev(), or register a /dev name
 * that differs from the one it advertises, and nothing else in the system
 * would notice.  This program turns that class of mistake into a boot-time
 * failure with a name attached.
 *
 * It also prints the device table, which is what makes `make test` output
 * useful when a driver silently fails to probe.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_DEVS 32

typedef struct {
    char name[32];
    char cls[16];
    char state[16];
    char dev[32];
    int  major, minor;
} entry_t;

static entry_t g_ent[MAX_DEVS];
static int     g_n;
static int     fails;

static void ok(const char *what, const char *who, int cond)
{
    printf("coldplug: %s %s: %s\n", what, who, cond ? "ok" : "FAIL");
    if (!cond)
        fails++;
}

/* /proc/subsystems is tab-separated: name, class, state, dev-or-"-",
 * major:minor.  Parsed by hand rather than with sscanf("%s") so that a
 * missing field is a parse failure instead of a silent shift by one. */
static int parse_line(char *line, entry_t *e)
{
    char *f[5];
    int   n = 0;
    char *p = line;

    while (n < 5) {
        f[n++] = p;
        char *t = strchr(p, '\t');
        if (!t)
            break;
        *t = 0;
        p = t + 1;
    }
    if (n != 5)
        return 0;

    snprintf(e->name,  sizeof e->name,  "%s", f[0]);
    snprintf(e->cls,   sizeof e->cls,   "%s", f[1]);
    snprintf(e->state, sizeof e->state, "%s", f[2]);
    snprintf(e->dev,   sizeof e->dev,   "%s", f[3]);
    e->major = e->minor = -1;
    sscanf(f[4], "%d:%d", &e->major, &e->minor);
    return 1;
}

static int load(void)
{
    FILE *f = fopen("/proc/subsystems", "r");
    if (!f) {
        printf("coldplug: cannot read /proc/subsystems: %s\n", strerror(errno));
        return 0;
    }

    char line[256];
    while (g_n < MAX_DEVS && fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (!line[0])
            continue;
        if (parse_line(line, &g_ent[g_n]))
            g_n++;
        else
            printf("coldplug: unparsable line: %s\n", line);
    }
    fclose(f);
    return 1;
}

int main(void)
{
    if (!load())
        return 1;

    printf("coldplug: %d subsystem(s) registered by the kernel\n", g_n);

    int live = 0, nodes = 0;
    for (int i = 0; i < g_n; i++) {
        entry_t *e = &g_ent[i];
        int is_live = strcmp(e->state, "live") == 0;
        int has_dev = strcmp(e->dev, "-") != 0;

        printf("coldplug:   %-10s %-8s %-10s %s",
               e->name, e->cls, e->state, has_dev ? "/dev/" : "(no node)");
        if (has_dev)
            printf("%s %d:%d", e->dev, e->major, e->minor);
        printf("\n");

        if (is_live)
            live++;
        if (!is_live || !has_dev)
            continue;
        nodes++;

        /* The reconciliation proper: the node the kernel advertises must be
         * there.  O_RDONLY, because a device that only accepts writes still
         * has to *exist*, and opening for read is the weakest thing that
         * proves it. */
        char path[64];
        snprintf(path, sizeof path, "/dev/%s", e->dev);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            printf("  %s: %s\n", path, strerror(errno));
        ok("node", path, fd >= 0);
        if (fd >= 0)
            close(fd);
    }

    /* The kernel has to have found *something*; an empty registry means the
     * registration path itself is broken, which every check above would
     * happily report as zero failures. */
    ok("registry", "is not empty", g_n > 0);
    ok("registry", "has live devices", live > 0);
    ok("registry", "publishes nodes", nodes > 0);

    /* The memory devices are the ones the rest of userland assumes without
     * checking, so name them explicitly rather than trusting the loop above
     * to have seen them. */
    static const char *required[] = { "null", "zero", "urandom", "tty" };
    for (unsigned i = 0; i < sizeof required / sizeof required[0]; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/%s", required[i]);
        int fd = open(path, O_RDONLY);
        ok("required", path, fd >= 0);
        if (fd >= 0)
            close(fd);
    }

    /* /dev/zero must actually read as zeroes, and /dev/urandom must not:
     * a stub that returns EOF for both would pass every open() above. */
    unsigned char buf[64];
    int fd = open("/dev/zero", O_RDONLY);
    if (fd >= 0) {
        memset(buf, 0xAA, sizeof buf);
        ssize_t n = read(fd, buf, sizeof buf);
        int all_zero = (n == (ssize_t)sizeof buf);
        for (ssize_t j = 0; j < n; j++)
            if (buf[j])
                all_zero = 0;
        ok("behaviour", "/dev/zero reads zeroes", all_zero);
        close(fd);
    }

    fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        unsigned char a[32], b[32];
        ssize_t n1 = read(fd, a, sizeof a);
        ssize_t n2 = read(fd, b, sizeof b);
        ok("behaviour", "/dev/urandom fills the buffer",
           n1 == (ssize_t)sizeof a && n2 == (ssize_t)sizeof b);
        ok("behaviour", "/dev/urandom does not repeat",
           memcmp(a, b, sizeof a) != 0);
        close(fd);
    }

    printf("coldplug: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
