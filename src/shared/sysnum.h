/*
 * sysnum.h — the kernel/user contract: syscall numbers, signals, flags.
 * (GPLv2)
 *
 * Included by both sides so the two can never drift apart.  The numbers and
 * the register convention follow Linux/x86-64 (nr in RAX, args in RDI, RSI,
 * RDX, R10, R8, R9; result in RAX; errors as negative errno) wherever a
 * Linux call means the same thing, so user code written against the familiar
 * interface behaves the way you would expect.  The two calls Linux does not
 * have are pushed up out of the way at 400.
 */
#ifndef GNUCOS_SYSNUM_H
#define GNUCOS_SYSNUM_H

#include <stdint.h>

/* Maximum length of a path the kernel will store or resolve.  Musl's own
 * PATH_MAX is 4096; 256 is plenty for a teaching OS and keeps per-open-file and
 * per-process cwd buffers small. */
#define GNUOS_PATH_MAX   256

#define SYS_read           0
#define SYS_write          1
#define SYS_open           2
#define SYS_close          3
#define SYS_stat           4
#define SYS_fstat          5
#define SYS_lstat          6
#define SYS_lseek          8
#define SYS_ioctl         16
#define SYS_readv         19
#define SYS_writev        20
#define SYS_pipe          22
#define SYS_sched_yield   24
#define SYS_dup           32
#define SYS_dup2          33
#define SYS_getpid        39
#define SYS_fork          57
#define SYS_vfork         58
#define SYS_execve        59
#define SYS_exit          60
#define SYS_wait4         61
#define SYS_kill          62
#define SYS_poll           7
#define SYS_ppoll        271
#define SYS_getcwd        79
#define SYS_chdir         80
#define SYS_fchdir        81
#define SYS_rename        82
#define SYS_mkdir         83
#define SYS_unlink        87
#define SYS_setpgid      109
#define SYS_getppid      110
#define SYS_getpgid      121
#define SYS_mprotect     10
#define SYS_mmap         9
#define SYS_munmap       11
#define SYS_brk          12
#define SYS_rt_sigaction 13
#define SYS_rt_sigprocmask 14
#define SYS_rt_sigreturn 15
#define SYS_rt_sigpending 127
#define SYS_tkill        200
#define SYS_tgkill       234
#define SYS_gettimeofday 96
#define SYS_getuid       102
#define SYS_getgid       104
#define SYS_geteuid      107
#define SYS_getegid      108
#define SYS_arch_prctl   158
#define SYS_gettid       186
#define SYS_futex        202
#define SYS_set_tid_address 218
#define SYS_clock_gettime 228
#define SYS_exit_group   231
#define SYS_madvise      28
#define SYS_getdents64   217
#define SYS_newfstatat   262
#define SYS_fcntl        72
#define SYS_openat       257
#define SYS_uname        63
#define SYS_umask        95
#define SYS_gethostname  100
#define SYS_sethostname  170
/* Interval timers.  musl has no alarm(2) of its own on x86-64: alarm() is
 * written in terms of setitimer(ITIMER_REAL), so a program as ordinary as
 * `ping` fails on a kernel that only implements 37. */
#define SYS_getitimer    36
#define SYS_alarm        37
#define SYS_setitimer    38
#define SYS_chmod        90
#define SYS_readlink     89
#define SYS_utimensat    280
#define SYS_access       21
#define SYS_faccessat    269
#define SYS_chown        92
#define SYS_fchown       93
#define SYS_lchown       94
#define SYS_signal       400          /* signal(sig, SIG_DFL|SIG_IGN) */
#define SYS_gstat        403          /* private: the old 24-byte gstat_t */

/* ---- sockets -----------------------------------------------------------
 * x86-64 has no socketcall(2) multiplexer -- that is a 32-bit i386 thing --
 * so musl issues every one of these as its own syscall number.  They all
 * have to be dispatched individually; a missing one surfaces in user space
 * as a bare ENOSYS with nothing to say which call went unanswered.
 */
#define SYS_socket        41
#define SYS_connect       42
#define SYS_accept        43
#define SYS_sendto        44
#define SYS_recvfrom      45
#define SYS_sendmsg       46
#define SYS_recvmsg       47
#define SYS_shutdown      48
#define SYS_bind          49
#define SYS_listen        50
#define SYS_getsockname   51
#define SYS_getpeername   52
#define SYS_socketpair    53
#define SYS_setsockopt    54
#define SYS_getsockopt    55
#define SYS_accept4      288

/* select(2) comes along with them: BusyBox's nc and wget multiplex with
 * select, not poll, and musl's select() is this call verbatim (pselect6 is
 * a separate number and is *not* what select() compiles to on x86-64). */
#define SYS_select        23
#define SYS_pselect6     270

/*
 * struct sockaddr_in as it crosses the syscall boundary: 16 bytes, and the
 * only place in this kernel where network byte order appears in a
 * user-visible structure.  sin_port and sin_addr are big-endian; everything
 * inside the kernel past the syscall layer is host order, so the conversion
 * happens exactly once, here at the edge.
 */
typedef struct {
    uint16_t sin_family;        /* AF_INET */
    uint16_t sin_port;          /* network order */
    uint32_t sin_addr;          /* network order */
    uint8_t  sin_zero[8];
} sockaddr_in_t;

/* arch_prctl() codes (x86-64). */
#define ARCH_SET_FS  0x1002
#define ARCH_GET_FS  0x1003

/* mmap() flags and prot bits (subsets Linux uses). */
#define PROT_READ    0x1
#define PROT_WRITE   0x2
#define PROT_EXEC    0x4
#define MAP_PRIVATE  0x02
#define MAP_FIXED    0x10
#define MAP_ANONYMOUS 0x20

/* futex() operations we recognise. */
#define FUTEX_WAIT   0
#define FUTEX_WAKE   1

/* signal() dispositions.  Anything else is the address of a handler. */
#define SIG_DFL  0
#define SIG_IGN  1

/*
 * sigaction() flags, Linux values.
 *
 * SA_RESTORER is the interesting one: on x86-64 the kernel does not own a
 * return trampoline, so the *caller* has to supply the few instructions that
 * turn "the handler returned" into rt_sigreturn().  musl always sets the flag
 * and points sa_restorer at its own __restore_rt, and we require the same --
 * the alternative is mapping a kernel-owned page into every address space for
 * the sake of nine bytes.
 */
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER  0x04000000

/* sigprocmask() operations. */
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

/* siginfo si_code: sent by kill() rather than by the kernel itself. */
#define SI_USER      0

/* Signals (Linux numbering). */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGKILL  9
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22

/* waitpid() options and status decoding. */
#define WNOHANG    1
#define WUNTRACED  2

#define WIFSTOPPED(s)   (((s) & 0xFF) == 0x7F)
#define WIFSIGNALED(s)  (((s) & 0x7F) != 0 && ((s) & 0xFF) != 0x7F)
#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WTERMSIG(s)     ((s) & 0x7F)
#define WSTOPSIG(s)     (((s) >> 8) & 0xFF)

/* open() flags.  The access mode is the low two bits, the rest are flags,
 * with the same values Linux uses so the numbers look familiar. */
#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0100
#define O_TRUNC    01000
#define O_APPEND   02000
#define O_NONBLOCK 04000

/* fcntl() commands musl actually issues for stdio/opendir. */
#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_DUPFD_CLOEXEC  1030
#define FD_CLOEXEC       1

/* ---- terminal control (ioctl(16)) --------------------------------------
 * Everything a terminal needs -- line discipline settings, window size, the
 * foreground process group -- goes through ioctl() with the same request
 * numbers Linux uses, because that is what musl's <termios.h> wrappers issue:
 * tcgetattr() is ioctl(fd, TCGETS, tio), tcsetattr(fd, act, tio) is
 * ioctl(fd, TCSETS + act, tio), and isatty() is a TIOCGWINSZ that succeeds.
 */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403      /* == TCSETS + TCSADRAIN */
#define TCSETSF     0x5404      /* == TCSETS + TCSAFLUSH */
#define TIOCSCTTY   0x540E
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414

/*
 * struct termios, in the *kernel's* x86-64 layout: 36 bytes, c_cc[19].
 *
 * This is deliberately not musl's user-space struct, which is 60 bytes
 * (c_cc[32] plus __c_ispeed/__c_ospeed).  The kernel layout is a strict
 * prefix of musl's, and musl never reads past it: cfget/cfsetospeed only
 * touch c_cflag & CBAUD, cfmakeraw() writes at most c_cc[VEOL2] (index 16),
 * and the two speed fields have no reader anywhere in musl's C code.  So
 * copying 36 bytes in either direction is safe, and it keeps us honest with
 * anything (BusyBox, strace output) that assumes the Linux ABI.
 */
#define GNUOS_NCCS 19

typedef struct {
    uint32_t c_iflag;           /* input modes    */
    uint32_t c_oflag;           /* output modes   */
    uint32_t c_cflag;           /* control modes  */
    uint32_t c_lflag;           /* local modes    */
    uint8_t  c_line;            /* line discipline (always 0 here) */
    uint8_t  c_cc[GNUOS_NCCS];  /* control characters */
} termios_t;

/* TIOCGWINSZ/TIOCSWINSZ payload.  Pixel dimensions are reported as 0. */
typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} winsize_t;

/* c_cc[] indices. */
#define VINTR     0
#define VQUIT     1
#define VERASE    2
#define VKILL     3
#define VEOF      4
#define VTIME     5
#define VMIN      6
#define VSWTC     7
#define VSTART    8
#define VSTOP     9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16

/* c_iflag bits. */
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IXON    0002000
#define IXANY   0004000
#define IXOFF   0010000
#define IMAXBEL 0020000
#define IUTF8   0040000

/* c_oflag bits. */
#define OPOST   0000001
#define ONLCR   0000004
#define OCRNL   0000010
#define ONOCR   0000020
#define ONLRET  0000040

/* c_cflag bits.  Only the baud field and CS8 mean anything to a frame
 * buffer, but programs read them back and expect what they set. */
#define CSIZE   0000060
#define CS8     0000060
#define CSTOPB  0000100
#define CREAD   0000200
#define PARENB  0000400
#define PARODD  0001000
#define HUPCL   0002000
#define CLOCAL  0004000
#define CBAUD   0010017
#define B0      0000000
#define B9600   0000015
#define B38400  0000017

/* c_lflag bits. */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define ECHOCTL 0001000
#define ECHOKE  0004000
#define IEXTEN  0100000

/* tcsetattr() actions, as the low bits of TCSETS/TCSETSW/TCSETSF. */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* pollfd event bits (Linux values), shared by poll(7)/ppoll(271). */
#define POLLIN    0x001
#define POLLPRI   0x002
#define POLLOUT   0x004
#define POLLERR   0x008
#define POLLHUP   0x010
#define POLLNVAL  0x020

/* errno values, as returned negated in RAX. */
#define EPERM      1
#define ENOENT     2
#define ESRCH      3
#define EINTR      4
#define EIO        5
#define EBADF      9
#define ECHILD    10
#define EAGAIN    11
#define ENOMEM    12
#define EFAULT    14
#define EEXIST    17
#define EXDEV     18
#define EISDIR    21
#define ENOTDIR   20
#define EINVAL    22
#define EMFILE    24
#define ENOTTY    25
#define ENOSPC    28
#define EPIPE     32
#define ENOSYS    38
#define ENOTEMPTY 39

/*
 * What a name refers to.  The numbers match the VFS's internal node kinds so
 * the kernel can hand its own value straight back.
 */
#define GK_FILE    1
#define GK_DIR     2
#define GK_CHARDEV 3
#define GK_PIPE    4

/* stat(): just enough to answer "does this exist, and what is it?" */
typedef struct {
    uint64_t size;
    uint32_t kind;              /* GK_* */
    uint32_t attr;              /* raw FAT attribute byte, 0 for devices */
} gstat_t;

/*
 * The Linux x86-64 struct stat musl and BusyBox decode.  The layout is copied
 * from musl's arch/x86_64/bits/stat.h: __pad0 is a 4-byte hole between st_gid
 * and st_rdev, and each timestamp is a {sec, nsec} pair.  It MUST stay 144
 * bytes -- the whole contract with user-space libc depends on it.
 */
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atim_sec;
    uint64_t st_atim_nsec;
    uint64_t st_mtim_sec;
    uint64_t st_mtim_nsec;
    uint64_t st_ctim_sec;
    uint64_t st_ctim_nsec;
    uint64_t __unused[3];
} lstat_t;

/*
 * Reading a directory descriptor yields a whole number of these records
 * instead of raw bytes.  Fixed-size records mean no getdents-style parsing
 * and no allocator in user space: read() into an array and you are done.
 */
#define GDIRENT_NAME 16

typedef struct {
    char     name[GDIRENT_NAME];
    uint32_t size;
    uint32_t kind;              /* GK_FILE or GK_DIR */
} gdirent_t;

/*
 * getdents64(217) record, in the layout musl's struct dirent expects.  The
 * kernel writes the fields with memcpy so alignment and strict-aliasing are
 * never an issue; d_reclen is rounded up to a multiple of 8 like Linux.
 */
typedef struct {
    uint64_t d_ino;
    uint64_t d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
} ldirent64_t;

/* d_type values for getdents64 records (match <dirent.h>). */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

/* newfstatat(262) flag bits. */
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH      0x1000
#define AT_FDCWD          (-100)

/*
 * uname(63).  Linux's new_utsname: six fixed 65-byte fields, no padding.
 * musl's struct utsname always reserves the sixth (as `domainname` under
 * _GNU_SOURCE and `__domainname` otherwise), so writing all 390 bytes is safe
 * whichever way the program was compiled.
 */
#define UTS_LEN  65
typedef struct {
    char sysname[UTS_LEN];
    char nodename[UTS_LEN];
    char release[UTS_LEN];
    char version[UTS_LEN];
    char machine[UTS_LEN];
    char domainname[UTS_LEN];
} utsname_t;

/* ---- auxv (AT_*) -------------------------------------------------------
 * Types passed to a new process on its initial stack, right after envp and
 * before AT_NULL.  libc's _start walks this list, so the values must match
 * what a Linux x86-64 loader would supply.
 */
#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14

#endif
