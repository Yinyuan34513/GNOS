/*
 * modtest.c -- loadable kernel module self-test. (GPLv2)
 *
 * Exercises the whole module path from user space, in order:
 *   1. finit_module() loads moddemo.ko from /lib/modules
 *   2. the module appears in /proc/modules and /proc/subsystems (it
 *      registered itself as a driver through the registry)
 *   3. a second load is refused with EEXIST
 *   4. modpair.ko loads, importing moddemo_ticks() from moddemo.ko --
 *      inter-module symbol resolution, which pins moddemo's refcount
 *   5. delete_module(moddemo) is refused with EBUSY while modpair holds
 *      the import
 *   6. after modpair goes away, moddemo unloads; /proc/subsystems forgets
 *      it; a second delete_module fails with ENOENT
 *   7. init_module() with garbage is refused with ENOEXEC
 *   8. init_module() (not finit_module) also works -- the name comes from
 *      the .ko's .modinfo section
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/stat.h>

#ifndef SYS_finit_module
#define SYS_finit_module 313
#endif

#define KO_DIR "/lib/modules"
#define KO_A   "/lib/modules/moddemo.ko"
#define KO_B   "/lib/modules/modpair.ko"

static int failures;

static void check(int ok, const char *what)
{
    printf("MODTEST: %s %s\n", ok ? "PASS" : "FAIL", what);
    syscall(441, what);
    if (!ok)
        failures++;
}

static int load_ko(const char *path, long *err_out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        *err_out = -errno;
        return -1;
    }
    long r = syscall(SYS_finit_module, fd, "", 0);
    *err_out = r ? -errno : 0;
    close(fd);
    return (int)r;
}

static int file_has(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f))
        if (strstr(line, needle)) {
            found = 1;
            break;
        }
    fclose(f);
    return found;
}

static void dump_proc_modules(void)
{
    FILE *f = fopen("/proc/modules", "r");
    if (!f)
        return;
    char line[256];
    printf("MODTEST: --- /proc/modules ---\n");
    while (fgets(line, sizeof(line), f))
        fputs(line, stdout);
    fclose(f);
}

int main(void)
{
    printf("MODTEST: starting\n");
    syscall(441, "MODTEST: starting");

    long err;
    int r = load_ko(KO_A, &err);
    check(r == 0, "PASS 1 (finit_module moddemo.ko)");

    /* Now visible in the module table and the driver registry. */
    check(file_has("/proc/modules", "moddemo") &&
          file_has("/proc/modules", "Live"),
          "PASS 2 (moddemo in /proc/modules, Live)");
    check(file_has("/proc/subsystems", "moddemo"),
          "PASS 3 (moddemo registered in /proc/subsystems)");

    /* Duplicate load refused. */
    r = load_ko(KO_A, &err);
    check(r < 0 && errno == EEXIST,
          "PASS 4 (second moddemo load -> EEXIST)");

    /* modpair imports a moddemo export: resolution + refcount pin. */
    r = load_ko(KO_B, &err);
    check(r == 0, "PASS 5 (finit_module modpair.ko)");
    dump_proc_modules();
    check(file_has("/proc/modules", "modpair"),
          "PASS 6 (modpair in /proc/modules)");

    /* moddemo is pinned by modpair's import. */
    r = syscall(SYS_delete_module, "moddemo", 0);
    check(r < 0 && errno == EBUSY,
          "PASS 7 (delete_module(moddemo) while imported -> EBUSY)");

    /* Unload order matters: modpair first, then moddemo. */
    r = syscall(SYS_delete_module, "modpair", 0);
    check(r == 0, "PASS 8 (delete_module(modpair))");
    r = syscall(SYS_delete_module, "moddemo", 0);
    check(r == 0, "PASS 9 (delete_module(moddemo))");
    check(!file_has("/proc/modules", "moddemo"),
          "PASS 10 (moddemo gone from /proc/modules)");
    check(!file_has("/proc/subsystems", "moddemo"),
          "PASS 11 (moddemo gone from /proc/subsystems)");

    /* Unloading what is not there. */
    r = syscall(SYS_delete_module, "moddemo", 0);
    check(r < 0 && errno == ENOENT,
          "PASS 12 (delete_module again -> ENOENT)");

    /* Garbage never loads. */
    static const char garbage[64] = "not an elf file at all";
    r = syscall(SYS_init_module, garbage, sizeof(garbage), "");
    check(r < 0 && errno == ENOEXEC,
          "PASS 13 (garbage init_module -> ENOEXEC)");

    /* init_module (no fd): name comes from .modinfo inside the .ko. */
    FILE *f = fopen(KO_A, "rb");
    if (!f) {
        check(0, "PASS 14 (open moddemo.ko for init_module)");
    } else {
        static char buf[1 << 20];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        r = syscall(SYS_init_module, buf, n, "");
        check(r == 0, "PASS 14 (init_module loads by .modinfo name)");
        r = syscall(SYS_delete_module, "moddemo", 0);
        check(r == 0, "PASS 15 (unload after init_module)");
    }

    if (failures)
        printf("MODTEST: done (%d FAILURES)\n", failures);
    else
        printf("MODTEST: done (ALL PASS)\n");
    syscall(441, failures ? "MODTEST: done (FAILURES)" : "MODTEST: done (ALL PASS)");
    return 0;
}