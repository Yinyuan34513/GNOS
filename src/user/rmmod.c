/*
 * rmmod.c -- unload a kernel module via delete_module(2). (GPLv2)
 *
 * Minimal Linux-flavoured rmmod.  -w retries while the module is busy
 * (an import pins it), -f forces the unload.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#define MODULE_DELETE_NONBLOCK 0x0001
#define MODULE_DELETE_FORCE    0x0002

static void usage(void)
{
    fprintf(stderr, "usage: rmmod [-f] [-w] module\n");
    exit(1);
}

int main(int argc, char **argv)
{
    unsigned int flags = 0;
    int wait_while_busy = 0;
    int opt;
    while ((opt = getopt(argc, argv, "fwsV")) != -1) {
        switch (opt) {
        case 'f':
            flags |= MODULE_DELETE_FORCE;
            break;
        case 'w':
            wait_while_busy = 1;
            break;
        case 's':                    /* syslog: we have no syslog */
            break;
        case 'V':
            printf("rmmod: GNOS rmmod 1.0\n");
            return 0;
        default:
            usage();
        }
    }
    if (optind >= argc)
        usage();
    const char *name = argv[optind];

    for (;;) {
        long r = syscall(SYS_delete_module, name, flags);
        if (r == 0)
            return 0;
        if (errno == EBUSY && wait_while_busy) {
            usleep(100000);
            continue;
        }
        fprintf(stderr, "rmmod: error removing '%s': %s\n", name,
                strerror(errno));
        return 1;
    }
}