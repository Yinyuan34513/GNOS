/*
 * ulib.h — the user-space runtime: syscall wrappers and a few string helpers.
 * (GPLv2)
 *
 * This is all the "libc" GNOS programs get.  Every function below is either
 * a thin wrapper around int 0x80 or something small enough that pulling in a
 * real library would cost more than writing it out.
 */
#ifndef GNUCOS_ULIB_H
#define GNUCOS_ULIB_H

#include <stdint.h>
#include <stddef.h>

#include "sysnum.h"

/* ---- raw system calls -------------------------------------------------- */
long sys_read(int fd, void *buf, long n);
long sys_write(int fd, const void *buf, long n);
/* dbgputs(441): copy a NUL-terminated string to the debug console. */
long sys_dbgputs(const char *s);
int  sys_open(const char *path, int flags);
int  sys_close(int fd);
int  sys_dup(int fd);
int  sys_dup2(int oldfd, int newfd);
long sys_lseek(int fd, long off, int whence);

/* Names: stat() is how you find out whether something exists *before* trying
 * to use it, which is the difference between "no such program" and a failed
 * exec that has already forked a process. */
int  sys_stat(const char *path, gstat_t *st);
int  mkdir(const char *path);
int  unlink(const char *path);

/* pipe(): fds[0] is the read end, fds[1] the write end. */
int  pipe(int fds[2]);

int  getpid(void);
int  getppid(void);
int  fork(void);
int  execv(const char *path, char *const argv[]);
int  execve(const char *path, char *const argv[], char *const envp[]);
void exit(int status) __attribute__((noreturn));
int  waitpid(int pid, int *status, int options);

int  kill(int pid, int sig);
int  signal(int sig, int disposition);

int  setpgid(int pid, int pgid);
int  getpgid(int pid);
void sched_yield(void);

/* ---- terminals ---------------------------------------------------------
 * All of these are ioctl() underneath, with the same request numbers a real
 * libc uses, so the kernel side is the Linux interface rather than a private
 * one.  tcsetpgrp() on a descriptor that is not the terminal fails with
 * ENOTTY, which is how a program can tell it has been redirected.
 */
int  ioctl(int fd, unsigned long req, void *arg);
int  tcsetpgrp(int fd, int pgid);
int  tcgetpgrp(int fd);
int  tcgetattr(int fd, termios_t *t);
int  tcsetattr(int fd, int action, const termios_t *t);
int  isatty(int fd);

/* ---- strings and memory ------------------------------------------------ */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strchr(const char *s, int c);
void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
int    atoi(const char *s);

/*
 * There is no current directory: the initrd is one flat FAT volume, so a
 * bare name always means a name in the root.  Every program that takes a
 * path funnels it through here so they all agree on that.
 */
const char *abspath(const char *name, char *buf, int cap);

/* ---- output ------------------------------------------------------------ */
void puts_fd(int fd, const char *s);      /* no newline appended */
void putn_fd(int fd, long v);
void print(const char *s);                /* to stdout */
void printn(long v);

#endif
