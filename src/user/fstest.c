/*
 * fstest -- headless checks for the ext2/VFS symlink + rename support that
 * OpenRC needs.  Driven from /etc/rc (no keyboard), so every check is a plain
 * assertion printed to the console.  Uses libc, not raw syscalls, so it
 * exercises the exact path musl takes (the *at() forms under the hood).
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

static int fails = 0;

static void ok(const char *name, int cond)
{
    printf("  %s: %s\n", name, cond ? "ok" : "FAIL");
    if (!cond)
        fails++;
}

#define TARGET "/tmp/fstest_target"
#define LINK   "/tmp/fstest_link"
#define REN    "/tmp/fstest_renamed"

int main(void)
{
    printf("fstest start\n");

    /* tidy any leftovers from a previous run */
    unlink(LINK);
    unlink(TARGET);
    unlink(REN);

    /* 1. a real file to point a symlink at */
    int fd = open(TARGET, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    ok("create target", fd >= 0);
    if (fd >= 0) {
        ok("write target", write(fd, "hi", 2) == 2);
        close(fd);
    }

    /* 2. symlink(2) */
    ok("symlink()", symlink(TARGET, LINK) == 0);

    /* 3. readlink(2): target text, no NUL terminator */
    char buf[256];
    ssize_t n = readlink(LINK, buf, sizeof(buf) - 1);
    buf[n > 0 ? (size_t)n : 0] = '\0';
    ok("readlink length", n == (ssize_t)strlen(TARGET));
    ok("readlink content", n > 0 && strcmp(buf, TARGET) == 0);

    /* 4. lstat sees a symlink, stat follows it to the regular file */
    struct stat lst, stt;
    ok("lstat link", lstat(LINK, &lst) == 0);
    ok("lstat is S_ISLNK", S_ISLNK(lst.st_mode));
    ok("stat follows", stat(LINK, &stt) == 0 && S_ISREG(stt.st_mode));
    ok("stat size 2", stt.st_size == 2);

    /* 5. rename(2) moves the file; the old name is gone, the new one present */
    ok("rename()", rename(TARGET, REN) == 0);
    ok("rename src gone", stat(TARGET, &stt) != 0);
    ok("rename dst present", stat(REN, &stt) == 0 && stt.st_size == 2);

    /* 6. the symlink now dangles: lstat still a symlink, stat fails ENOENT */
    ok("dangling lstat S_ISLNK", lstat(LINK, &lst) == 0 && S_ISLNK(lst.st_mode));
    errno = 0;
    ok("dangling stat ENOENT",
       stat(LINK, &stt) != 0 && errno == ENOENT);

    /* 7. a relative symlink resolves against the link's own directory */
    unlink(TARGET);                 /* re-create target under a shorter name */
    fd = open(TARGET, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);
    char *rel = "/tmp/fstest_rel";
    unlink(rel);
    ok("symlink relative", symlink("fstest_target", rel) == 0);
    ok("relative stat follows", stat(rel, &stt) == 0 && S_ISREG(stt.st_mode));

    /* cleanup */
    unlink(LINK);
    unlink(rel);
    unlink(TARGET);
    unlink(REN);

    printf("fstest: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
