#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

int main(void)
{
    struct winsize w;
    errno = 0;
    int r = ioctl(0, TIOCGWINSZ, &w);
    printf("ioctl(TIOCGWINSZ): r=%d errno=%d (%s)\n", r, errno, strerror(errno));

    errno = 0;
    char *t = ttyname(0);
    printf("ttyname: \"%s\" errno=%d (%s)\n", t ? t : "", errno, strerror(errno));

    errno = 0;
    r = isatty(0);
    printf("isatty: %d errno=%d (%s)\n", r, errno, strerror(errno));
    return 0;
}