/*
 * hello.c — the smallest possible musl-linked program.
 *
 * Its only job is to prove that the whole musl toolchain (crt1.o, libc.a,
 * TLS setup via arch_prctl, the syscall gate) works end to end on GNOS:
 * print a line and exit cleanly.
 */
#include <stdio.h>

int main(void)
{
    printf("Hello from GNOS, via musl libc!\n");
    return 0;
}
