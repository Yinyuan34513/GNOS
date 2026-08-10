/*
 * count.c — a long-running program to try job control against. (GPLv2)
 *
 * Prints a line, burns some time, repeats.  That is enough to see every
 * piece of the machinery: run it in the foreground and ^C or ^Z it, run it
 * with & and watch it keep printing while the shell still takes commands,
 * then bring it back with fg.
 */
#include "ulib.h"

#define DEFAULT_ROUNDS  20
#define BUSY_ITERATIONS 60000000L

static long atol_(const char *s)
{
    long v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

int main(int argc, char **argv)
{
    long rounds = (argc > 1) ? atol_(argv[1]) : DEFAULT_ROUNDS;
    if (rounds <= 0)
        rounds = DEFAULT_ROUNDS;

    for (long i = 1; i <= rounds; i++) {
        print("count[");
        printn(getpid());
        print("] ");
        printn(i);
        print("/");
        printn(rounds);
        print("\n");

        /* No clock syscall yet, so waste time the honest way.  volatile
         * keeps the optimiser from deleting the whole loop. */
        for (volatile long k = 0; k < BUSY_ITERATIONS; k++)
            ;
    }

    print("count[");
    printn(getpid());
    print("] finished\n");
    return 0;
}
