/*
 * insmod.c -- load a kernel module via finit_module(2). (GPLv2)
 *
 * Minimal Linux-flavoured insmod: opens the .ko, hands the fd to the
 * kernel.  Options are accepted for compatibility with scripts that use
 * them; -f is the only one that changes behaviour (it skips the
 * vermagic/modversions checks, which GNOS does not perform anyway).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifndef SYS_finit_module
#define SYS_finit_module 313
#endif

#define MODULE_INIT_IGNORE_MODVERSIONS 0x0001
#define MODULE_INIT_IGNORE_VERMAGIC    0x0002

static void usage(void)
{
    fprintf(stderr, "usage: insmod [-f] module.ko\n");
    exit(1);
}

int main(int argc, char **argv)
{
    unsigned int flags = 0;
    int opt;
    while ((opt = getopt(argc, argv, "fkpvsV")) != -1) {
        switch (opt) {
        case 'f':
            flags |= MODULE_INIT_IGNORE_MODVERSIONS |
                     MODULE_INIT_IGNORE_VERMAGIC;
            break;
        case 'k':                    /* autoclean: GNOS unloads only on demand */
        case 'p':                    /* probe: nothing to probe here         */
        case 's':                    /* syslog: we have no syslog            */
            break;
        case 'V':
            printf("insmod: GNOS insmod 1.0\n");
            return 0;
        default:
            usage();
        }
    }
    if (optind >= argc)
        usage();
    const char *path = argv[optind];

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "insmod: can't open '%s': %s\n", path,
                strerror(errno));
        return 1;
    }
    long r = syscall(SYS_finit_module, fd, "", flags);
    if (r != 0) {
        fprintf(stderr, "insmod: error inserting '%s': %s\n", path,
                strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}