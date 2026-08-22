/*
 * dynhello.c — dynamically linked hello for GNOS. (GPLv2)
 *
 * Compiled with musl-gcc *without* -static, so the resulting binary is a
 * PIE (ET_DYN) with a PT_INTERP pointing at /lib/ld-musl-x86_64.so.1.  Its
 * existence on the boot assertions is the whole point: it proves the kernel
 * loader can map a PIE, find and load the dynamic linker, publish
 * AT_BASE/AT_ENTRY/AT_PHDR correctly, and that musl's dynlink stage then
 * resolves libc.so and reaches main().  Nothing about this file itself is
 * interesting; the linkage is.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv;
    printf("DYNHELLO: dynamically linked hello from GNOS\n");
    int n = 0;
    for (; envp[n]; n++)
        printf("DYNEV[%d]=%s\n", n, envp[n]);
    printf("DYNEV total %d\n", n);
    const char *x = getenv("XKB_CONFIG_ROOT");
    printf("DYNGETENV XKB_CONFIG_ROOT=%s\n", x ? x : "(null)");
    return 0;
}
