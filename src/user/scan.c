/*
 * scan.c — tiny diagnostic: open each /dev/sdX and report. (GPLv2)
 */
#include <stdint.h>
#include "ulib.h"

#define O_RDWR 2
#define BLKGETSIZE64 0x80081272

int main(void)
{
    for (int i = 0; i < 4; i++) {
        char name[16];
        name[0] = '/'; name[1] = 'd'; name[2] = 'e'; name[3] = 'v';
        name[4] = '/'; name[5] = 's'; name[6] = 'd'; name[7] = 'a' + i;
        name[8] = 0;

        print("open ");
        print(name);
        int fd = sys_open(name, O_RDWR);
        print(" -> ");
        printn(fd);
        if (fd < 0)
            continue;

        uint64_t sz = 0;
        long r = ioctl(fd, BLKGETSIZE64, &sz);
        print("  ioctl -> ");
        printn(r);
        print("  bytes=");
        printn((long)sz);
        print("\n");
    }
    return 0;
}
