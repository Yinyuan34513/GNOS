/*
 * mounttest.c — headless assertions for mount(2) + getrandom(2). (GPLv2)
 *
 * Verifies, with no human at the keyboard, that the kernel's mount syscall
 * can put a tmpfs on a directory and that the tmpfs then behaves like a real
 * filesystem (write/read/mkdir/unlink/readdir all route into it), and that
 * getrandom(2) returns varying non-zero bytes.  Driven from /etc/rc like the
 * other *test programs, so a regression fails `make test` instead of waiting
 * for someone to notice it at a prompt.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <errno.h>

#ifndef __NR_mount
#define __NR_mount 165
#endif
#ifndef __NR_umount2
#define __NR_umount2 166
#endif
#ifndef __NR_getrandom
#define __NR_getrandom 318
#endif

static int fails = 0;

static void ok(const char *name, int cond)
{
    printf("mounttest: %s: %s\n", name, cond ? "ok" : "FAIL");
    if (!cond)
        fails++;
}

int main(void)
{
    char buf[64];

    /* 1. mount a tmpfs at /mnt (the initrd already has an empty /mnt). */
    long r = syscall(__NR_mount, NULL, "/mnt", "tmpfs", 0UL, NULL);
    ok("mount tmpfs /mnt", r == 0);
    if (r != 0)
        printf("  mount returned %ld\n", r);

    /* 2. write a file on the tmpfs. */
    FILE *f = fopen("/mnt/hello", "w");
    ok("fopen /mnt/hello (write)", f != NULL);
    if (f) {
        fputs("tmpfs works", f);
        fclose(f);
    }

    /* 3. read it back and check the contents survived. */
    f = fopen("/mnt/hello", "r");
    ok("fopen /mnt/hello (read)", f != NULL);
    if (f) {
        memset(buf, 0, sizeof buf);
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        ok("tmpfs read content", n == 11 && memcmp(buf, "tmpfs works", 11) == 0);
    }

    /* 4. mkdir + readdir enumerate the tmpfs directory. */
    r = mkdir("/mnt/sub", 0755);
    ok("mkdir /mnt/sub", r == 0);

    int saw_hello = 0, saw_sub = 0;
    DIR *d = opendir("/mnt");
    ok("opendir /mnt", d != NULL);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, "hello") == 0)
                saw_hello = 1;
            if (strcmp(de->d_name, "sub") == 0)
                saw_sub = 1;
        }
        closedir(d);
    }
    ok("readdir sees hello", saw_hello);
    ok("readdir sees sub", saw_sub);

    /* 5. unlink removes the file from the tmpfs. */
    r = unlink("/mnt/hello");
    ok("unlink /mnt/hello", r == 0);

    /* 6. getrandom: non-zero and different on successive calls. */
    unsigned char g1[16], g2[16];
    long gr = syscall(__NR_getrandom, g1, sizeof g1, 0UL);
    ok("getrandom length", gr == 16);
    int nonzero = 0;
    for (int i = 0; i < 16; i++)
        if (g1[i])
            nonzero = 1;
    ok("getrandom non-zero", nonzero);
    syscall(__NR_getrandom, g2, sizeof g2, 0UL);
    int differs = 0;
    for (int i = 0; i < 16; i++)
        if (g1[i] != g2[i])
            differs = 1;
    ok("getrandom varies", differs);

    /* 7. umount releases the tmpfs. */
    r = syscall(__NR_umount2, "/mnt", 0UL);
    ok("umount /mnt", r == 0);

    /* 8. after umount, /mnt is the (empty) ext2 directory again. */
    d = opendir("/mnt");
    ok("opendir /mnt after umount", d != NULL);
    if (d) {
        struct dirent *de;
        int count = 0;
        while ((de = readdir(d)) != NULL) {
            printf("mounttest:   after-umount entry: '%s' (type %d)\n",
                   de->d_name, de->d_type);
            count++;
        }
        closedir(d);
        printf("mounttest:   after-umount count: %d\n", count);
        ok("ext2 /mnt empty again", count == 2); /* "." and ".." only */
    }

    printf("mounttest: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
