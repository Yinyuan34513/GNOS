/*
 * mount.c — a small mount(8) for the GNOS userland. (GPLv2)
 *
 * The kernel knows one mountable filesystem, tmpfs, so this is deliberately
 * narrow.  What it does have to get right is the *shapes* of the command line
 * that an init system uses, because OpenRC's init.sh and its localmount
 * service invoke all four:
 *
 *   mount                       list what is mounted (from /proc/mounts)
 *   mount -t tmpfs none /run    an explicit mount
 *   mount /run                  a mount described by /etc/fstab
 *   mount -a [-t types]         every fstab entry that is not mounted yet
 *
 * The last two are why /etc/fstab is a real file here and not documentation:
 * `mount -a` is how the boot brings up /run, /tmp and /dev/shm, and if this
 * program ignored fstab then adding a line to it would silently do nothing.
 *
 * A request for a filesystem the kernel does not implement (proc, sysfs,
 * devtmpfs) returns ENODEV, which OpenRC treats as "skip and continue" -- the
 * right outcome, since /proc is already there and /dev is static.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

#ifndef __NR_mount
#define __NR_mount 165
#endif

#define FSTAB "/etc/fstab"

static int do_mount(const char *source, const char *target, const char *fstype,
                    int quiet)
{
    long r = syscall(__NR_mount, source ? source : "", target, fstype, 0UL, NULL);
    if (r != 0) {
        if (!quiet)
            fprintf(stderr, "mount: %s on %s: %s\n", fstype, target,
                    strerror(errno));
        return 1;
    }
    return 0;
}

/* Is `target` listed in /proc/mounts?  `mount -a` must not re-mount what is
 * already there: stacking a second tmpfs on /run would hide the pidfiles the
 * first one is holding. */
static int already_mounted(const char *target)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return 0;
    char line[256];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char dev[128], dir[128];
        if (sscanf(line, "%127s %127s", dev, dir) == 2 &&
            strcmp(dir, target) == 0)
            found = 1;
    }
    fclose(f);
    return found;
}

/* One fstab record.  Fields are the classic six; we only need the first
 * three, and only the ones with a type the kernel can actually mount. */
typedef struct {
    char src[128];
    char dir[128];
    char type[32];
    char opts[128];
} fstab_t;

static int fstab_next(FILE *f, fstab_t *e)
{
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h)
            *h = 0;
        e->opts[0] = 0;
        int n = sscanf(line, "%127s %127s %31s %127s",
                       e->src, e->dir, e->type, e->opts);
        if (n >= 3)
            return 1;
    }
    return 0;
}

/* `-t` on `mount -a` takes a comma-separated list, optionally negated with a
 * leading "no" (mount -a -t nosysfs).  Both forms show up in service scripts,
 * so both are honoured rather than guessed at. */
static int type_selected(const char *want, const char *type)
{
    if (!want)
        return 1;

    int negate = 0;
    if (strncmp(want, "no", 2) == 0) {
        negate = 1;
        want += 2;
    }

    int match = 0;
    const char *p = want;
    while (*p && !match) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == strlen(type) && strncmp(p, type, len) == 0)
            match = 1;
        p = comma ? comma + 1 : p + len;
    }
    return negate ? !match : match;
}

static int mount_all(const char *want_type)
{
    FILE *f = fopen(FSTAB, "r");
    if (!f) {
        /* No fstab is not an error: there is simply nothing to replay. */
        return 0;
    }

    fstab_t e;
    int rc = 0;
    while (fstab_next(f, &e)) {
        if (strcmp(e.dir, "/") == 0 || strcmp(e.dir, "none") == 0)
            continue;                        /* the root is already mounted */
        if (!type_selected(want_type, e.type))
            continue;
        if (already_mounted(e.dir))
            continue;
        /* Quietly: an fstab that lists proc or sysfs is describing a normal
         * Linux system, and the entries this kernel cannot honour should not
         * turn a successful boot into a wall of errors. */
        if (do_mount(e.src, e.dir, e.type, 1) != 0 && strcmp(e.type, "tmpfs") == 0)
            rc = 1;                          /* a tmpfs failure is real */
    }
    fclose(f);
    return rc;
}

/* `mount /run` -- everything but the mount point comes from fstab. */
static int mount_from_fstab(const char *target)
{
    FILE *f = fopen(FSTAB, "r");
    if (!f) {
        fprintf(stderr, "mount: can't find %s in %s\n", target, FSTAB);
        return 1;
    }
    fstab_t e;
    int rc = 1;
    while (fstab_next(f, &e)) {
        if (strcmp(e.dir, target) != 0 && strcmp(e.src, target) != 0)
            continue;
        rc = do_mount(e.src, e.dir, e.type, 0);
        break;
    }
    fclose(f);
    if (rc == 1)
        fprintf(stderr, "mount: can't find %s in %s\n", target, FSTAB);
    return rc;
}

static int list_mounts(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return 0;
    char dev[128], dir[128], type[64], opts[128];
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%127s %127s %63s %127s", dev, dir, type, opts) >= 3)
            printf("%s on %s type %s (%s)\n", dev, dir, type,
                   opts[0] ? opts : "defaults");
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *fstype = NULL;
    const char *source = NULL;
    const char *target = NULL;
    int all = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-t") == 0 && i + 1 < argc) {
            fstype = argv[++i];
        } else if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            i++;                    /* options are accepted and ignored */
        } else if (strcmp(a, "-a") == 0) {
            all = 1;
        } else if (strcmp(a, "-n") == 0 ||
                   strcmp(a, "-r") == 0 ||
                   strcmp(a, "-v") == 0 ||
                   strcmp(a, "-f") == 0) {
            /* no-mtab / read-only / verbose / fake: nothing to act on here */
        } else if (a[0] == '-' && a[1]) {
            /* unknown single-dash flag: swallow it and any value it takes */
            if (i + 1 < argc && argv[i + 1][0] != '-')
                i++;
        } else {
            /* positional: first is the source, second the target. */
            if (!source)
                source = a;
            else
                target = a;
        }
    }

    if (all)
        return mount_all(fstype);

    /* Bare `mount` is the "list what is mounted" form. */
    if (!source && !target && !fstype)
        return list_mounts();

    /* One positional argument is a mount point to look up in fstab. */
    if (source && !target && !fstype)
        return mount_from_fstab(source);

    if (!target) {
        fprintf(stderr, "mount: missing mount point\n");
        return 1;
    }
    if (!fstype)
        return mount_from_fstab(target);

    return do_mount(source, target, fstype, 0);
}
