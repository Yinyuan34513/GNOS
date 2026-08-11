/*
 * syscall.c — the int 0x80 POSIX gate. (GPLv2)
 *
 * Everything a user process can ask the kernel to do arrives here with the
 * Linux/x86-64 register convention: number in RAX, arguments in RDI, RSI,
 * RDX, R10, R8, R9, result back in RAX, failures as a negative errno.
 *
 * File descriptors are per-process small integers that index the PCB's fd
 * table; the values stored there are handles into the VFS's global open-file
 * table.  The indirection is what makes fork() and dup2() cheap and what
 * gives a parent and child a genuinely shared file offset.
 */
#include <stddef.h>
#include <stdint.h>

#include "syscall.h"
#include "idt.h"
#include "gdt.h"
#include "vfs.h"
#include "proc.h"
#include "sock.h"
#include "net.h"
#include "signal.h"
#include "vmm.h"
#include "tty.h"
#include "timer.h"
#include "pmm.h"
#include "panic.h"
#include "kstring.h"
#include "debugcon.h"

/* The `syscall` instruction lands here; defined in isr.asm.  It builds a
 * regs_t frame and calls syscall_handler(), exactly like the int 0x80 path. */
extern uint8_t syscall_entry[];

static void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

/* Room for a shell's environment plus a long command line.  bash alone
 * exports about 30 variables before it runs anything. */
#define MAX_ARGV    128

/* user_ptr_ok() now lives in vmm.c (the network ioctl path needs it too);
 * it is declared in vmm.h, which this file includes. */

/* ---- path resolution --------------------------------------------------
 *
 * Every path a program hands us is turned into a normalised absolute path
 * here, before the VFS ever sees it.  Doing it at this layer rather than
 * inside the VFS has two payoffs: the VFS stays a pure "absolute names only"
 * lookup with no notion of a process, and p->cwd is canonical by
 * construction, so getcwd() is a memcpy and fchdir() can trust the string a
 * descriptor was opened with.
 *
 * "Normalised" means lexical: "." is dropped, ".." pops the previous
 * component, and runs of slashes collapse.  There are no symlinks on this
 * filesystem, so a lexical walk and a real one cannot disagree.
 */
static int path_norm(const char *base, const char *path, char *out)
{
    if (!path)
        return -E_INVAL;

    /* Splice base and path into one string first, so the walk below has a
     * single buffer to chew through. */
    char     buf[GNUOS_PATH_MAX];
    uint32_t n = 0;

    if (path[0] != '/') {
        for (const char *b = base; *b; b++) {
            if (n >= GNUOS_PATH_MAX - 1)
                return -E_NAMETOOLONG;
            buf[n++] = *b;
        }
        if (n == 0 || buf[n - 1] != '/') {
            if (n >= GNUOS_PATH_MAX - 1)
                return -E_NAMETOOLONG;
            buf[n++] = '/';
        }
    }
    for (const char *s = path; *s; s++) {
        if (n >= GNUOS_PATH_MAX - 1)
            return -E_NAMETOOLONG;
        buf[n++] = *s;
    }
    buf[n] = 0;

    /* Rebuild it component by component.  out always starts with the root
     * slash, and a separator is only added between components, which is what
     * keeps "/" itself from coming back as "//". */
    uint32_t o = 0;
    out[o++] = '/';

    for (uint32_t i = 0; i < n; ) {
        while (i < n && buf[i] == '/')
            i++;
        uint32_t start = i;
        while (i < n && buf[i] != '/')
            i++;
        uint32_t len = i - start;

        if (len == 0)
            break;
        if (len == 1 && buf[start] == '.')
            continue;
        if (len == 2 && buf[start] == '.' && buf[start + 1] == '.') {
            while (o > 1 && out[o - 1] != '/')
                o--;
            if (o > 1)
                o--;                   /* drop the separator as well */
            continue;                  /* ".." at the root is the root */
        }

        if (o > 1) {
            if (o >= GNUOS_PATH_MAX - 1)
                return -E_NAMETOOLONG;
            out[o++] = '/';
        }
        if (o + len >= GNUOS_PATH_MAX)
            return -E_NAMETOOLONG;
        memcpy(out + o, buf + start, len);
        o += len;
    }

    out[o] = 0;
    return 0;
}

/* Resolve a user path against the calling process's current directory. */
static int path_abs(uint64_t upath, char *out)
{
    if (!user_ptr_ok(upath, 1))
        return -E_INVAL;

    proc_t *p = proc_current();
    return path_norm(p ? p->cwd : "/", (const char *)(uintptr_t)upath, out);
}

static int fd_handle(int fd);

/*
 * Resolve the (dirfd, path) pair the *at() syscalls take.  AT_FDCWD means the
 * current directory; any other descriptor supplies its own path as the base,
 * which is why the open-file table remembers the name it was opened with.
 */
static int path_at(int dfd, uint64_t upath, char *out)
{
    if (!user_ptr_ok(upath, 1))
        return -E_INVAL;

    const char *path = (const char *)(uintptr_t)upath;
    if (path[0] == '/' || dfd == AT_FDCWD)
        return path_abs(upath, out);

    const char *dir = vfs_file_path(fd_handle(dfd));
    if (!dir)
        return -E_BADF;
    return path_norm(dir, path, out);
}

/* ---- descriptors ------------------------------------------------------ */
static int fd_handle(int fd)
{
    proc_t *p = proc_current();
    if (!p || fd < 0 || fd >= PROC_MAX_FD)
        return -1;
    return p->fds[fd];
}

static int fd_alloc(proc_t *p, int handle)
{
    for (int fd = 0; fd < PROC_MAX_FD; fd++) {
        if (p->fds[fd] < 0) {
            p->fds[fd] = handle;
            /* A recycled descriptor number must not inherit the previous
             * tenant's close-on-exec flag. */
            p->fd_cloexec &= ~(1ULL << fd);
            return fd;
        }
    }
    return -E_MFILE;
}

/* Shared tail of open()/openat(): open an already-resolved path and give the
 * process the lowest free descriptor for it. */
static int64_t open_resolved(const char *abs, int flags)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;

    /* O_CLOEXEC describes the descriptor, not the open file, so it is
     * stripped here and never reaches the filesystem layer. */
    int cloexec = (flags & O_CLOEXEC) != 0;
    flags &= ~O_CLOEXEC;

    int h = vfs_file_open(abs, flags);
    if (h < 0)
        return h;

    int fd = fd_alloc(p, h);
    if (fd < 0) {
        vfs_file_unref(h);
        return fd;
    }
    if (cloexec)
        p->fd_cloexec |= 1ULL << fd;
    return fd;
}

static int64_t sys_open(uint64_t upath, int flags)
{
    char abs[GNUOS_PATH_MAX];
    int  r = path_abs(upath, abs);
    if (r < 0)
        return r;
    return open_resolved(abs, flags);
}

/*
 * openat(257): musl routes every open() through this with AT_FDCWD, so a musl
 * program cannot open a file without it.
 */
static int64_t sys_openat(int dfd, uint64_t upath, int flags)
{
    char abs[GNUOS_PATH_MAX];
    int  r = path_at(dfd, upath, abs);
    if (r < 0)
        return r;
    return open_resolved(abs, flags);
}

/*
 * fcntl(72).  opendir() and the stdio layer probe/set FD_CLOEXEC here, and a
 * shell depends on it for real: every fd it keeps for its own bookkeeping
 * (the saved terminal, the script it is reading, the fd a here-document
 * lives on) is marked close-on-exec so the command it runs cannot see them.
 * The flag is per-descriptor, not per-open-file, so it lives in the process
 * bitmap rather than in the file table -- dup()ing a cloexec fd gives you a
 * second one that is *not* cloexec, which is exactly how a shell moves a
 * descriptor out of the way before exec.
 */
static int64_t sys_fcntl(int fd, int cmd, uint64_t arg)
{
    int h = fd_handle(fd);
    if (h < 0)
        return -E_BADF;

    proc_t *p = proc_current();
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        if (!p)
            return -E_BADF;
        int newfd = (int)arg;
        if (newfd < 0 || newfd >= PROC_MAX_FD)
            return -E_INVAL;
        while (newfd < PROC_MAX_FD && p->fds[newfd] >= 0)
            newfd++;
        if (newfd >= PROC_MAX_FD)
            return -E_MFILE;
        p->fds[newfd] = h;
        vfs_file_ref(h);
        if (cmd == F_DUPFD_CLOEXEC)
            p->fd_cloexec |= 1ULL << newfd;
        else
            p->fd_cloexec &= ~(1ULL << newfd);
        return newfd;
    }
    case F_GETFD:
        if (!p)
            return -E_BADF;
        return (p->fd_cloexec & (1ULL << fd)) ? FD_CLOEXEC : 0;
    case F_SETFD:
        if (!p)
            return -E_BADF;
        if (arg & FD_CLOEXEC)
            p->fd_cloexec |= 1ULL << fd;
        else
            p->fd_cloexec &= ~(1ULL << fd);
        return 0;
    case F_GETFL: {
        /* Access mode (the low two bits) plus, for sockets, the O_NONBLOCK
         * bit that lives in the socket itself.  musl and BusyBox read this to
         * decide whether a connect()/accept()/recv() will block. */
        int fl = vfs_file_flags(h) & 3;
        int s = vfs_file_sock(h);
        if (s >= 0 && sock_is_nonblock(s))
            fl |= O_NONBLOCK;
        return fl;
    }
    case F_SETFL: {
        int s = vfs_file_sock(h);
        if (s >= 0)
            sock_set_nonblock(s, (arg & O_NONBLOCK) != 0);
        /* Track O_NONBLOCK the same way the kernel does for files: keep the
         * access mode and flip only the settable bit. */
        vfs_file_setfl(h, (int)(arg & O_NONBLOCK));
        return 0;
    }
    default:       return -E_NOSYS;
    }
}

static int64_t sys_close(int fd)
{
    proc_t *p = proc_current();
    if (!p || fd < 0 || fd >= PROC_MAX_FD || p->fds[fd] < 0)
        return -E_BADF;

    vfs_file_unref(p->fds[fd]);
    p->fds[fd] = -1;
    p->fd_cloexec &= ~(1ULL << fd);
    return 0;
}

/* dup(32): the lowest free descriptor onto the same open file. */
static int64_t sys_dup(int oldfd)
{
    proc_t *p = proc_current();
    if (!p || oldfd < 0 || oldfd >= PROC_MAX_FD || p->fds[oldfd] < 0)
        return -E_BADF;

    int fd = fd_alloc(p, p->fds[oldfd]);
    if (fd < 0)
        return fd;
    vfs_file_ref(p->fds[oldfd]);
    return fd;
}

static int64_t do_dup2(int oldfd, int newfd, int cloexec)
{
    proc_t *p = proc_current();
    if (!p || oldfd < 0 || oldfd >= PROC_MAX_FD || p->fds[oldfd] < 0)
        return -E_BADF;
    if (newfd < 0 || newfd >= PROC_MAX_FD)
        return -E_BADF;
    if (oldfd == newfd)
        return newfd;

    if (p->fds[newfd] >= 0)
        vfs_file_unref(p->fds[newfd]);
    p->fds[newfd] = p->fds[oldfd];
    vfs_file_ref(p->fds[newfd]);
    /* The new descriptor gets its own flag; the old one keeps whatever it
     * had.  A shell dup2()s a cloexec fd onto 0/1/2 precisely because the
     * copy is *not* close-on-exec. */
    if (cloexec)
        p->fd_cloexec |= 1ULL << newfd;
    else
        p->fd_cloexec &= ~(1ULL << newfd);
    return newfd;
}

static int64_t sys_dup2(int oldfd, int newfd)
{
    return do_dup2(oldfd, newfd, 0);
}

/*
 * dup3(292): dup2 with a flags word.  musl's dup2 falls back to this on
 * architectures without a real dup2, and O_CLOEXEC is the only flag
 * defined.  Unlike dup2, dup3 with oldfd == newfd is an error, not a no-op.
 */
static int64_t sys_dup3(int oldfd, int newfd, int flags)
{
    if (flags & ~O_CLOEXEC)
        return -E_INVAL;
    if (oldfd == newfd)
        return -E_INVAL;
    return do_dup2(oldfd, newfd, (flags & O_CLOEXEC) != 0);
}

/* ---- names ------------------------------------------------------------ */
/*
 * The private 24-byte gstat_t, reachable as SYS_gstat(403).  The toy userland
 * (ulib.c) still calls this; nothing else should.  Linux's own SYS_stat/fstat/
 * lstat/newfstatat now return the 144-byte lstat_t below instead.
 */
static int64_t sys_gstat(uint64_t upath, uint64_t ust)
{
    if (!user_ptr_ok(ust, sizeof(gstat_t)))
        return -E_INVAL;

    char abs[GNUOS_PATH_MAX];
    int  r = path_abs(upath, abs);
    if (r < 0)
        return r;

    uint64_t size = 0;
    int      kind = 0;
    r = vfs_stat(abs, &size, &kind);
    if (r < 0)
        return r;

    gstat_t *st = (gstat_t *)(uintptr_t)ust;
    st->size = size;
    st->kind = (uint32_t)kind;
    st->attr = 0;
    return 0;
}

/* Linux-style stat from a path.  `follow` selects stat() (1, chase the final
 * symlink) vs lstat() (0, report the symlink itself). */
static int64_t sys_stat(uint64_t upath, uint64_t ust, int follow)
{
    if (!user_ptr_ok(ust, sizeof(lstat_t)))
        return -E_INVAL;

    char abs[GNUOS_PATH_MAX];
    int  r = path_abs(upath, abs);
    if (r < 0)
        return r;
    return vfs_stat_linux(abs, (lstat_t *)(uintptr_t)ust, follow);
}

/* Linux-style fstat from a descriptor (SYS_fstat=5). */
static int64_t sys_fstat(int fd, uint64_t ust)
{
    if (!user_ptr_ok(ust, sizeof(lstat_t)))
        return -E_INVAL;
    return vfs_fstat(fd_handle(fd), (lstat_t *)(uintptr_t)ust);
}

/* readlink()/readlinkat(): copy the link target into `ubuf` (up to `usize`
 * bytes, no NUL terminator) and return the target length. */
static int64_t do_readlink(const char *abs, uint64_t ubuf, uint64_t usize)
{
    char kbuf[4096];
    int  n = vfs_readlink(abs, kbuf, sizeof(kbuf));
    if (n < 0)
        return n;

    uint64_t w = (usize < (uint64_t)n) ? usize : (uint64_t)n;
    if (w && user_ptr_ok(ubuf, w))
        memcpy((void *)(uintptr_t)ubuf, kbuf, w);
    else if (w)
        return -E_INVAL;
    return (int64_t)n;
}

static int64_t sys_readlink(const char *upath, uint64_t ubuf, uint64_t usize)
{
    char abs[GNUOS_PATH_MAX];
    int  r = path_abs((uint64_t)(uintptr_t)upath, abs);
    if (r < 0)
        return r;
    return do_readlink(abs, ubuf, usize);
}

/*
 * newfstatat(262): (dirfd, path, buf, flags).  AT_EMPTY_PATH stats the dirfd
 * itself; anything else is resolved against the dirfd (or the cwd).
 */
static int64_t sys_newfstatat(int fd, uint64_t upath, uint64_t ust, uint64_t flags)
{
    if (!user_ptr_ok(ust, sizeof(lstat_t)))
        return -E_INVAL;

    if (flags & AT_EMPTY_PATH)
        return vfs_fstat(fd_handle(fd), (lstat_t *)(uintptr_t)ust);

    char abs[GNUOS_PATH_MAX];
    int  r = path_at(fd, upath, abs);
    if (r < 0)
        return r;
    return vfs_stat_linux(abs, (lstat_t *)(uintptr_t)ust,
                          (flags & AT_SYMLINK_NOFOLLOW) ? 0 : 1);
}

/* ---- the current directory -------------------------------------------- */
/* Adopt an already-resolved path as the cwd, provided it really is one. */
static int64_t cwd_set(const char *abs)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;

    uint64_t size = 0;
    int      kind = 0;
    int r = vfs_stat(abs, &size, &kind);
    if (r < 0)
        return r;
    if (kind != VFS_DIR)
        return -E_NOTDIR;

    strncpy(p->cwd, abs, GNUOS_PATH_MAX - 1);
    p->cwd[GNUOS_PATH_MAX - 1] = 0;
    return 0;
}

static int64_t sys_chdir(uint64_t upath)
{
    char abs[GNUOS_PATH_MAX];
    int  r = path_abs(upath, abs);
    if (r < 0)
        return r;
    return cwd_set(abs);
}

static int64_t sys_fchdir(int fd)
{
    const char *dir = vfs_file_path(fd_handle(fd));
    if (!dir)
        return -E_BADF;
    return cwd_set(dir);
}

/*
 * getcwd(79).  Linux returns the length *including* the terminator, and
 * ERANGE when the buffer is too small -- musl checks only for a negative
 * return, but glibc-compiled code relies on the length, so report it.
 */
static int64_t sys_getcwd(uint64_t ubuf, uint64_t size)
{
    proc_t *p = proc_current();
    if (!p || size == 0 || !user_ptr_ok(ubuf, size))
        return -E_INVAL;

    uint64_t need = strlen(p->cwd) + 1;
    if (size < need)
        return -E_RANGE;

    memcpy((void *)(uintptr_t)ubuf, p->cwd, need);
    return (int64_t)need;
}

/* ---- odds and ends a full libc expects -------------------------------- */
/* Hostname, a single small buffer.  Processes are all root here, so there is
 * no permission check to skip. */
static char g_hostname[64] = "gnos";

/*
 * uname(63).  The strings are what a program prints, and occasionally what a
 * configure-style script branches on, so they name this kernel honestly
 * rather than impersonating Linux.
 */
static int64_t sys_uname(uint64_t ubuf)
{
    if (!user_ptr_ok(ubuf, sizeof(utsname_t)))
        return -E_INVAL;

    utsname_t *u = (utsname_t *)(uintptr_t)ubuf;
    memset(u, 0, sizeof(*u));
    strncpy(u->sysname,  "GNOS",     UTS_LEN - 1);
    strncpy(u->nodename, g_hostname,  UTS_LEN - 1);
    strncpy(u->release,  "0.1",       UTS_LEN - 1);
    strncpy(u->version,  "GNOS 0.1", UTS_LEN - 1);
    strncpy(u->machine,  "x86_64",    UTS_LEN - 1);
    return 0;
}

static int64_t sys_sethostname(uint64_t uname, uint64_t len)
{
    if (len > sizeof(g_hostname) - 1)
        return -E_INVAL;
    if (!user_ptr_ok(uname, len))
        return -E_FAULT;
    memset(g_hostname, 0, sizeof(g_hostname));
    memcpy(g_hostname, (const void *)(uintptr_t)uname, len);
    /* Drop a trailing NUL if the caller included one. */
    if (len && g_hostname[len - 1] == '\0')
        g_hostname[len - 1] = '\0';
    return 0;
}

static int64_t sys_gethostname(uint64_t ubuf, uint64_t sz)
{
    if (sz < 1)
        return -E_INVAL;
    if (!user_ptr_ok(ubuf, 1))
        return -E_FAULT;
    size_t n = strlen(g_hostname);
    size_t copy = (n < (size_t)sz) ? n : (size_t)sz - 1;
    char *u = (char *)(uintptr_t)ubuf;
    memcpy(u, g_hostname, copy);
    if (copy < (size_t)sz)
        u[copy] = '\0';
    return 0;
}

/* ---- getrandom(318) ---------------------------------------------------
 * Fill `buf` with `usize` pseudo-random bytes.  We have no hardware entropy
 * source, so this is a fast xorshift64 stream seeded from the TSC plus a
 * global counter -- "insecure" by construction, but perfectly good for the
 * things a boot actually uses it for (ASLR-ish, seedrng's seed, temp names).
 * The GRND_* flags are all honoured trivially: we never block.
 */
static uint64_t g_rng_state;
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static uint64_t rng_u64(void)
{
    uint64_t x = g_rng_state;
    if (x == 0)
        x = 0x9E3779B97F4A7C15ULL;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng_state = x;
    return x;
}
void krandom_bytes(void *buf, uint32_t n)
{
    if (g_rng_state == 0)
        g_rng_state = rdtsc() ^ 0x9E3779B97F4A7C15ULL;
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < n) {
        uint64_t w = rng_u64();
        uint32_t chunk = n - done;
        if (chunk > 8)
            chunk = 8;
        memcpy(p + done, &w, chunk);
        done += chunk;
    }
}

static int64_t sys_getrandom(uint64_t ubuf, uint64_t usize, uint64_t uflags)
{
    (void)uflags;
    if (usize == 0)
        return 0;
    /* Cap to keep a huge request from faulting on a user buffer we only
     * range-checked at the start.  64 MB is far above anything reasonable. */
    if (usize > 64u * 1024 * 1024)
        usize = 64u * 1024 * 1024;
    if (!user_ptr_ok(ubuf, 1))
        return -E_FAULT;
    if (g_rng_state == 0)
        g_rng_state = rdtsc() ^ 0x9E3779B97F4A7C15ULL;

    uint8_t *u = (uint8_t *)(uintptr_t)ubuf;
    size_t n = (size_t)usize;
    size_t done = 0;
    while (done < n) {
        uint64_t w = rng_u64();
        size_t chunk = n - done;
        if (chunk > 8)
            chunk = 8;
        memcpy(u + done, &w, chunk);
        done += chunk;
    }
    return (int64_t)n;
}

/* ---- mount(165) / umount2(166) ---------------------------------------
 * Only tmpfs is mountable; everything else (proc/sysfs/devtmpfs) is either
 * already special-cased by the VFS (/proc) or intentionally left alone
 * (/dev must keep its static device nodes).  MS_REMOUNT is accepted as a
 * no-op so shutdown-style "mount -o remount,ro" does not fail the boot.
 */
#define MS_REMOUNT 0x20

static int64_t sys_mount(uint64_t usrc, uint64_t utgt, uint64_t ufs,
                         uint64_t uflags, uint64_t udata)
{
    (void)usrc;
    (void)udata;

    if (!user_ptr_ok(utgt, 1))
        return -E_FAULT;
    char tgt[GNUOS_PATH_MAX];
    strncpy(tgt, (const char *)(uintptr_t)utgt, GNUOS_PATH_MAX - 1);
    tgt[GNUOS_PATH_MAX - 1] = 0;

    char fst[32];
    if (ufs) {
        if (!user_ptr_ok(ufs, 1))
            return -E_FAULT;
        strncpy(fst, (const char *)(uintptr_t)ufs, sizeof(fst) - 1);
        fst[sizeof(fst) - 1] = 0;
    } else {
        fst[0] = 0;
    }

    char abs[GNUOS_PATH_MAX];
    int r = path_norm("/", tgt, abs);
    if (r < 0)
        return r;

    if (uflags & MS_REMOUNT)
        return 0;                       /* ro/rw tracking is not implemented */

    if (strcmp(fst, "tmpfs") != 0)
        return -E_NODEV;

    return vfs_mount_tmpfs(abs);
}

static int64_t sys_umount2(uint64_t utgt, uint64_t uflags)
{
    (void)uflags;
    if (!user_ptr_ok(utgt, 1))
        return -E_FAULT;
    char tgt[GNUOS_PATH_MAX];
    strncpy(tgt, (const char *)(uintptr_t)utgt, GNUOS_PATH_MAX - 1);
    tgt[GNUOS_PATH_MAX - 1] = 0;
    char abs[GNUOS_PATH_MAX];
    int r = path_norm("/", tgt, abs);
    if (r < 0)
        return r;
    return vfs_umount(abs);
}

/* ---- interval timers --------------------------------------------------
 * setitimer(38)/getitimer(36)/alarm(37), ITIMER_REAL only.
 *
 * musl has no alarm(2) on x86-64 -- it writes alarm() in terms of
 * setitimer(ITIMER_REAL) -- so without 38 a program as ordinary as `ping`
 * dies on ENOSYS before it sends a packet.  ITIMER_VIRTUAL and ITIMER_PROF
 * need per-process CPU-time accounting the scheduler does not keep, so they
 * are refused outright rather than quietly aliased onto real time, which
 * would make a profiler report confident nonsense.
 */
#define ITIMER_REAL     0

/* ktimeval_t is the shared struct timeval from sysnum.h. */
typedef struct { ktimeval_t it_interval; ktimeval_t it_value; } kitimerval_t;

#define USEC_PER_TICK (1000000ULL / SCHED_HZ)

/* Round up: a sub-tick delay truncated to zero would read back as
 * "disarmed" and the signal would never arrive. */
static uint64_t tv_to_ticks(const ktimeval_t *tv)
{
    return (uint64_t)tv->tv_sec * SCHED_HZ +
           ((uint64_t)tv->tv_usec + USEC_PER_TICK - 1) / USEC_PER_TICK;
}

static void ticks_to_tv(uint64_t ticks, ktimeval_t *tv)
{
    tv->tv_sec  = (int64_t)(ticks / SCHED_HZ);
    tv->tv_usec = (int64_t)(ticks % SCHED_HZ) * (int64_t)USEC_PER_TICK;
}

/* How long is left on p's timer, in ticks; 0 when it is not armed. */
static uint64_t itimer_remaining(const proc_t *p, uint64_t now)
{
    if (!p->itimer_expire || p->itimer_expire <= now)
        return 0;
    return p->itimer_expire - now;
}

static int64_t sys_setitimer(uint64_t which, uint64_t unew, uint64_t uold)
{
    if (which != ITIMER_REAL)
        return -E_INVAL;

    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;

    /* Copy the new setting in before writing the old one out: callers are
     * allowed to pass the same buffer for both, and overwriting it first
     * would arm the timer from the value we just clobbered. */
    kitimerval_t nv;
    if (unew) {
        if (!user_ptr_ok(unew, sizeof(nv)))
            return -E_FAULT;
        memcpy(&nv, (const void *)(uintptr_t)unew, sizeof(nv));
        if (nv.it_value.tv_sec < 0 || nv.it_value.tv_usec < 0 ||
            nv.it_interval.tv_sec < 0 || nv.it_interval.tv_usec < 0 ||
            nv.it_value.tv_usec >= 1000000 || nv.it_interval.tv_usec >= 1000000)
            return -E_INVAL;
    }

    uint64_t now = timer_ticks();

    if (uold) {
        if (!user_ptr_ok(uold, sizeof(kitimerval_t)))
            return -E_FAULT;
        kitimerval_t ov;
        ticks_to_tv(itimer_remaining(p, now), &ov.it_value);
        ticks_to_tv(p->itimer_interval, &ov.it_interval);
        memcpy((void *)(uintptr_t)uold, &ov, sizeof(ov));
    }

    if (unew) {
        uint64_t val = tv_to_ticks(&nv.it_value);
        p->itimer_interval = tv_to_ticks(&nv.it_interval);
        p->itimer_expire   = val ? now + val : 0;
    }
    return 0;
}

static int64_t sys_getitimer(uint64_t which, uint64_t ucur)
{
    if (which != ITIMER_REAL)
        return -E_INVAL;
    if (!user_ptr_ok(ucur, sizeof(kitimerval_t)))
        return -E_FAULT;

    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;

    kitimerval_t cur;
    ticks_to_tv(itimer_remaining(p, timer_ticks()), &cur.it_value);
    ticks_to_tv(p->itimer_interval, &cur.it_interval);
    memcpy((void *)(uintptr_t)ucur, &cur, sizeof(cur));
    return 0;
}

/* alarm(37).  Returns the seconds left on the previous alarm, rounded up as
 * Linux does, so `remaining = alarm(0)` never reports 0 for a live timer. */
static int64_t sys_alarm(uint64_t seconds)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;

    uint64_t now  = timer_ticks();
    uint64_t left = (itimer_remaining(p, now) + 99) / 100;

    p->itimer_interval = 0;
    p->itimer_expire   = seconds ? now + seconds * 100 : 0;
    return (int64_t)left;
}

/* umask(95): remembered and inherited, but never enforced -- we have no
 * permission checks to enforce it against.  Programs still expect the
 * read-back-and-restore idiom to work. */
static int64_t sys_umask(uint32_t mask)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;

    uint32_t old = p->umask;
    p->umask = mask & 0777;
    return (int64_t)old;
}

static int64_t sys_chmod(uint64_t upath, uint32_t mode)
{
    char abs[GNUOS_PATH_MAX];
    int  r = path_abs(upath, abs);
    if (r < 0)
        return r;
    return vfs_chmod(abs, mode);
}

/*
 * access(21) / faccessat(269).  Existence is the only question we can answer
 * honestly: there is one user, no credentials to check a file's mode against,
 * and no enforcement anywhere in the VFS.  So a file that is there is a file
 * this process may read, write and execute, whatever R_OK/W_OK/X_OK asked
 * for -- which is also the truth, given that every open() succeeds.
 */
static int64_t sys_access(int dfd, uint64_t upath, int at)
{
    char abs[GNUOS_PATH_MAX];
    int  r = at ? path_at(dfd, upath, abs) : path_abs(upath, abs);
    if (r < 0)
        return r;

    lstat_t st;
    return vfs_stat_linux(abs, &st, 1);
}

/*
 * getresuid(118)/getresgid(120): every r/e/s uid/gid is 0 (single root user).
 * Each pointer may be NULL; write 0 only through the non-NULL ones.
 */
static int64_t sys_getresuid(uint64_t ruid, uint64_t euid, uint64_t suid)
{
    uint32_t z = 0;
    if (ruid && user_ptr_ok(ruid, 4)) *(uint32_t *)(uintptr_t)ruid = z;
    if (euid && user_ptr_ok(euid, 4)) *(uint32_t *)(uintptr_t)euid = z;
    if (suid && user_ptr_ok(suid, 4)) *(uint32_t *)(uintptr_t)suid = z;
    return 0;
}

static int64_t sys_getresgid(uint64_t rgid, uint64_t egid, uint64_t sgid)
{
    uint32_t z = 0;
    if (rgid && user_ptr_ok(rgid, 4)) *(uint32_t *)(uintptr_t)rgid = z;
    if (egid && user_ptr_ok(egid, 4)) *(uint32_t *)(uintptr_t)egid = z;
    if (sgid && user_ptr_ok(sgid, 4)) *(uint32_t *)(uintptr_t)sgid = z;
    return 0;
}

/* getdents64(217): fill a musl struct dirent buffer from a directory fd. */
static int64_t sys_getdents64(int fd, uint64_t ubuf, uint64_t len)
{
    if (!user_ptr_ok(ubuf, len))
        return -E_INVAL;
    return vfs_dir_getdents64(fd_handle(fd), (void *)(uintptr_t)ubuf,
                              (uint32_t)len);
}

/* ---- pipes ------------------------------------------------------------ */
/*
 * pipe() is the only call that hands back two descriptors, and the only one
 * that has to undo half its work: if the second fd cannot be allocated the
 * first must go back, or a shell that hits the limit leaks a pipe end and
 * its reader never sees end-of-file.
 */
static int64_t sys_pipe2(uint64_t ufds, int flags)
{
    proc_t *p = proc_current();
    if (!p || !user_ptr_ok(ufds, 2 * sizeof(int)))
        return -E_INVAL;
    /* O_NONBLOCK is accepted but does nothing: the pipe's read and write
     * paths live below the descriptor layer and always block.  Rejecting it
     * would be worse -- callers pass it opportunistically and treat the
     * failure as fatal. */
    if (flags & ~(O_CLOEXEC | O_NONBLOCK))
        return -E_INVAL;

    int rh, wh;
    int r = vfs_pipe(&rh, &wh);
    if (r < 0)
        return r;

    int rfd = fd_alloc(p, rh);
    if (rfd < 0) {
        vfs_file_unref(rh);
        vfs_file_unref(wh);
        return rfd;
    }

    int wfd = fd_alloc(p, wh);
    if (wfd < 0) {
        p->fds[rfd] = -1;
        vfs_file_unref(rh);
        vfs_file_unref(wh);
        return wfd;
    }

    if (flags & O_CLOEXEC)
        p->fd_cloexec |= (1ULL << rfd) | (1ULL << wfd);

    int *out = (int *)(uintptr_t)ufds;
    out[0] = rfd;
    out[1] = wfd;
    return 0;
}

static int64_t sys_pipe(uint64_t ufds)
{
    return sys_pipe2(ufds, 0);
}

/* ---- sockets -----------------------------------------------------------
 *
 * This is a translation layer and nothing else: user pointers become kernel
 * values, network byte order becomes host order, and a socket index becomes a
 * file descriptor.  Every decision about what a socket *does* lives in sock.c.
 *
 * Two of Linux's conventions are worth naming, because both are easy to get
 * subtly wrong and neither fails loudly.  First, an address length arrives by
 * value on the way in (bind, connect, sendto) but by pointer on the way out
 * (accept, recvfrom, getsockname), because the out direction has to say how
 * much room the answer needed.  Second, what it reports is the *full* size of
 * the address even when the caller's buffer was too small: the address gets
 * truncated, the length does not.  getaddrinfo() checks that number.
 */

/* fd -> socket index, or a negative errno.  ENOTSOCK rather than EBADF for a
 * descriptor that exists but is a file: that distinction is the only clue a
 * program gets that it passed the wrong fd rather than a closed one. */
static int fd_sock(int fd)
{
    int h = fd_handle(fd);
    if (h < 0)
        return -E_BADF;
    int s = vfs_file_sock(h);
    return s < 0 ? -E_NOTSOCK : s;
}

/* Read a sockaddr_in in; `alen` is a plain length. */
static int sa_in(uint64_t uaddr, uint64_t alen, uint32_t *ip, uint16_t *port)
{
    if (!uaddr || alen < sizeof(sockaddr_in_t))
        return -E_INVAL;
    if (!user_ptr_ok(uaddr, sizeof(sockaddr_in_t)))
        return -E_FAULT;

    sockaddr_in_t sa;
    memcpy(&sa, (const void *)(uintptr_t)uaddr, sizeof(sa));
    if (sa.sin_family != AF_INET)
        return -E_AFNOSUPPORT;

    *ip   = net_ntohl(sa.sin_addr);
    *port = net_ntohs(sa.sin_port);
    return 0;
}

/* Write a sockaddr_in out.  A null address or length pointer is not an
 * error: it is how accept() and recvfrom() say "I do not care who it was". */
static int sa_out(uint64_t uaddr, uint64_t ualen, uint32_t ip, uint16_t port)
{
    if (!uaddr || !ualen)
        return 0;
    if (!user_ptr_ok(ualen, sizeof(uint32_t)))
        return -E_FAULT;

    uint32_t room;
    memcpy(&room, (const void *)(uintptr_t)ualen, sizeof(room));
    if (room > sizeof(sockaddr_in_t))
        room = (uint32_t)sizeof(sockaddr_in_t);
    if (room && !user_ptr_ok(uaddr, room))
        return -E_FAULT;

    sockaddr_in_t sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = net_htons(port);
    sa.sin_addr   = net_htonl(ip);
    if (room)
        memcpy((void *)(uintptr_t)uaddr, &sa, room);

    uint32_t full = (uint32_t)sizeof(sockaddr_in_t);
    memcpy((void *)(uintptr_t)ualen, &full, sizeof(full));
    return 0;
}

/*
 * Hand a freshly created socket to the process.  On failure the socket is
 * closed here rather than leaked: by this point it may already own a TCP
 * connection, and nobody else has a name for it.
 */
static int64_t sock_to_fd(int s)
{
    proc_t *p = proc_current();
    if (!p) {
        sock_close(s);
        return -E_INVAL;
    }

    int h = vfs_socket(s);
    if (h < 0) {
        sock_close(s);
        return h;
    }

    int fd = fd_alloc(p, h);
    if (fd < 0) {
        vfs_file_unref(h);            /* the last unref closes the socket */
        return fd;
    }
    return fd;
}

static int64_t sys_socket(int domain, int type, int protocol)
{
    int s = sock_create(domain, type, protocol);
    if (s < 0)
        return s;
    return sock_to_fd(s);
}

static int64_t sys_bind(int fd, uint64_t uaddr, uint64_t alen)
{
    int s = fd_sock(fd);
    if (s < 0)
        return s;

    uint32_t ip;
    uint16_t port;
    int r = sa_in(uaddr, alen, &ip, &port);
    return r < 0 ? r : sock_bind(s, ip, port);
}

static int64_t sys_connect(int fd, uint64_t uaddr, uint64_t alen)
{
    int s = fd_sock(fd);
    if (s < 0)
        return s;

    uint32_t ip;
    uint16_t port;
    int r = sa_in(uaddr, alen, &ip, &port);
    return r < 0 ? r : sock_connect(s, ip, port);
}

static int64_t sys_listen(int fd, int backlog)
{
    int s = fd_sock(fd);
    return s < 0 ? s : sock_listen(s, backlog);
}

/* accept(43) is accept4(288) with no flags. */
static int64_t sys_accept(int fd, uint64_t uaddr, uint64_t ualen, int flags)
{
    int s = fd_sock(fd);
    if (s < 0)
        return s;

    uint32_t ip   = 0;
    uint16_t port = 0;
    int ns = sock_accept(s, &ip, &port);
    if (ns < 0)
        return ns;
    if (flags & SOCK_NONBLOCK)
        sock_set_nonblock(ns, 1);

    int64_t nfd = sock_to_fd(ns);
    if (nfd < 0)
        return nfd;

    /* The connection is up and the descriptor exists; a bad address pointer
     * cannot un-accept either, so the fd is the answer regardless. */
    sa_out(uaddr, ualen, ip, port);
    return nfd;
}

static int64_t sys_sendto(int fd, uint64_t ubuf, uint64_t len, int flags,
                          uint64_t uaddr, uint64_t alen)
{
    int s = fd_sock(fd);
    if (s < 0)
        return s;
    if (len && !user_ptr_ok(ubuf, len))
        return -E_FAULT;

    uint32_t ip   = 0;
    uint16_t port = 0;
    if (uaddr) {
        int r = sa_in(uaddr, alen, &ip, &port);
        if (r < 0)
            return r;
    }
    return sock_sendto(s, (const void *)(uintptr_t)ubuf, (uint32_t)len, flags,
                       ip, port);
}

static int64_t sys_recvfrom(int fd, uint64_t ubuf, uint64_t len, int flags,
                            uint64_t uaddr, uint64_t ualen)
{
    int s = fd_sock(fd);
    if (s < 0)
        return s;
    if (len && !user_ptr_ok(ubuf, len))
        return -E_FAULT;

    uint32_t ip   = 0;
    uint16_t port = 0;
    int n = sock_recvfrom(s, (void *)(uintptr_t)ubuf, (uint32_t)len, flags,
                          &ip, &port);
    if (n < 0)
        return n;

    int r = sa_out(uaddr, ualen, ip, port);
    return r < 0 ? r : n;
}

static int64_t sys_shutdown(int fd, int how)
{
    int s = fd_sock(fd);
    return s < 0 ? s : sock_shutdown(s, how);
}

/* getsockname(51) and getpeername(52) differ only in which end they name. */
static int64_t sys_getsockname(int fd, uint64_t uaddr, uint64_t ualen, int peer)
{
    int s = fd_sock(fd);
    if (s < 0)
        return s;
    if (!uaddr || !ualen)
        return -E_INVAL;

    uint32_t ip   = 0;
    uint16_t port = 0;
    int r = sock_getname(s, &ip, &port, peer);
    return r < 0 ? r : sa_out(uaddr, ualen, ip, port);
}

/* Option values are small -- every one anybody sets is an int -- so a fixed
 * bounce buffer is enough and keeps user memory out of sock.c entirely. */
#define SOCKOPT_MAX 128

static int64_t sys_setsockopt(int fd, int level, int name, uint64_t uval,
                              uint64_t len)
{
    int s = fd_sock(fd);
    if (s < 0)
        return s;
    if (len > SOCKOPT_MAX)
        return -E_INVAL;
    if (len && !user_ptr_ok(uval, len))
        return -E_FAULT;

    uint8_t tmp[SOCKOPT_MAX];
    if (len)
        memcpy(tmp, (const void *)(uintptr_t)uval, (uint32_t)len);
    return sock_setsockopt(s, level, name, tmp, (uint32_t)len);
}

static int64_t sys_getsockopt(int fd, int level, int name, uint64_t uval,
                              uint64_t ulen)
{
    int s = fd_sock(fd);
    if (s < 0)
        return s;
    if (!user_ptr_ok(ulen, sizeof(uint32_t)))
        return -E_FAULT;

    uint32_t room;
    memcpy(&room, (const void *)(uintptr_t)ulen, sizeof(room));
    if (room > SOCKOPT_MAX)
        room = SOCKOPT_MAX;
    if (!user_ptr_ok(uval, room))
        return -E_FAULT;

    uint8_t  tmp[SOCKOPT_MAX];
    uint32_t got = room;
    int r = sock_getsockopt(s, level, name, tmp, &got);
    if (r < 0)
        return r;

    if (got > room)
        got = room;
    memcpy((void *)(uintptr_t)uval, tmp, got);
    memcpy((void *)(uintptr_t)ulen, &got, sizeof(got));
    return 0;
}

/* ---- execve argument vector ------------------------------------------- */
/*
 * Validate one of the user's NULL-terminated pointer vectors and flatten it
 * into a kernel-side array.  The strings themselves stay in user memory:
 * proc_execve() copies them out while the old address space is still the
 * current one.
 *
 * A vector longer than the array is E2BIG rather than a silent truncation.
 * Quietly dropping the tail of an environment is the kind of bug that shows
 * up much later as a program mysteriously ignoring $PATH.
 */
static char *g_argv[MAX_ARGV + 1];
static char *g_envp[MAX_ARGV + 1];

static int collect_vec(uint64_t uvec, char **dst)
{
    dst[0] = NULL;
    if (!uvec)
        return 0;
    if (!user_ptr_ok(uvec, 8))
        return -E_INVAL;

    char *const *ua = (char *const *)(uintptr_t)uvec;

    int n = 0;
    for (; n <= MAX_ARGV; n++) {
        if (!user_ptr_ok((uint64_t)(uintptr_t)&ua[n], 8))
            return -E_INVAL;
        char *s = ua[n];
        if (!s)
            break;
        if (n == MAX_ARGV)
            return -E_2BIG;
        if (!user_ptr_ok((uint64_t)(uintptr_t)s, 1))
            return -E_INVAL;
        dst[n] = s;
    }
    dst[n] = NULL;
    return n;
}

/* ---- signals ---------------------------------------------------------- */
static int64_t sys_kill(int pid, int sig)
{
    proc_t *me = proc_current();
    if (!me)
        return -E_SRCH;

    if (pid > 0) {
        proc_t *t = proc_by_pid(pid);
        if (!t)
            return -E_SRCH;
        return proc_signal(t, sig);
    }

    int pgid = (pid == 0) ? me->pgid : -pid;
    return proc_signal_group(pgid, sig) ? 0 : -E_SRCH;
}

/*
 * tkill/tgkill are what musl's raise() and pthread_kill() issue (tkill is
 * 200, tgkill is 234 on x86-64).  They name a specific thread; in this
 * single-threaded kernel a thread id is just a pid, so both collapse to the
 * same proc_signal() as kill().
 */
static int64_t sys_tkill(int tid, int sig)
{
    if (tid <= 0)
        return -E_INVAL;
    proc_t *t = proc_by_pid((uint32_t)tid);
    if (!t)
        return -E_SRCH;
    return proc_signal(t, sig);
}

static int64_t sys_tgkill(int tgid, int tid, int sig)
{
    (void)tgid;                        /* no thread groups to tell apart */
    return sys_tkill(tid, sig);
}

/*
 * poll(7) / ppoll(271) / select(23).  The event sources are the keyboard
 * tty, pipes and sockets; everything else (a file on disk) is always ready,
 * because a read of it cannot block.
 *
 * This is the shared core: `p` points at `nfds` pollfd records of the layout
 * {int32_t fd; int16_t events; int16_t revents} (8 bytes each).  The caller
 * has already made the buffer readable -- sys_ppoll/sys_poll pass a user
 * buffer (the kernel reads user memory directly), while sys_select passes a
 * kernel scratch array it built itself, so no user_ptr_ok check belongs here.
 * `ticks` is the timeout in timer ticks; -1 means wait forever, 0 means probe
 * and return immediately.
 */
static int64_t do_ppoll(uint8_t *p, uint64_t nfds, int64_t ticks)
{
    if (nfds > 4096)
        nfds = 4096;
    if (nfds == 0)
        return 0;

    int64_t ready = 0;
    for (;;) {
        int wait_tty = 0;
        int wait_net = 0;
        ready = 0;
        net_poll();                   /* fold any received packet into socket buffers */
        for (uint64_t i = 0; i < nfds; i++) {
            int32_t  fd = *(int32_t  *)(p + i*8 + 0);
            int16_t  ev = *(int16_t  *)(p + i*8 + 4);
            int16_t *rv = (int16_t  *)(p + i*8 + 6);
            int16_t  r  = 0;

            if (fd < 0) {
                r = POLLNVAL;
            } else {
                int h = fd_handle(fd);
                if (h < 0) {
                    r = POLLNVAL;
                } else {
                    uint8_t kind = vfs_file_kind(h);
                    if (kind == VFS_CHARDEV) {
                        if (tty_input_avail() > 0)
                            r = POLLIN;
                        wait_tty = 1;
                    } else if (kind == VFS_PIPE) {
                        if (vfs_pipe_readable(h))
                            r = POLLIN;
                    } else if (kind == VFS_SOCKET) {
                        int s = vfs_file_sock(h);
                        if (s >= 0) {
                            if ((ev & POLLIN)  && sock_readable(s))  r |= POLLIN;
                            if ((ev & POLLOUT) && sock_writable(s))  r |= POLLOUT;
                        }
                        wait_net = 1;
                    } else {
                        /* regular files are always readable/writable */
                        r = (int16_t)(ev & (POLLIN | POLLOUT));
                    }
                }
            }
            *rv = r;
            if (r)
                ready++;
        }

        if (ready > 0 || ticks == 0)
            break;

        /* Nothing is ready and we may block.  Sleep until the relevant event
         * source changes, then re-scan.  A pending signal makes us return
         * EINTR, just like a real kernel. */
        proc_t *cur = proc_current();
        if (!cur)
            break;
        if (proc_pending_signals(cur))
            return -E_INTR;

        if (wait_tty)
            sched_block_timeout(WAIT_TTY, ticks < 0 ? 0 : (uint64_t)ticks);
        else if (wait_net)
            sched_block_timeout(WAIT_NET, ticks < 0 ? 0 : (uint64_t)ticks);
        else
            sched_block_timeout(WAIT_PIPE, ticks < 0 ? 0 : (uint64_t)ticks);

        if (proc_pending_signals(cur))
            return -E_INTR;
    }

    return ready;
}

/* Convert a ppoll timespec (user pointer, possibly NULL) into timer ticks.
 * -1 == wait forever, 0 == probe only.  Returns a negative errno on a bad
 * user pointer. */
static int ppoll_timeout(uint64_t utmo, int64_t *ticks)
{
    *ticks = -1;
    if (!utmo)
        return 0;
    if (!user_ptr_ok(utmo, 16))
        return -E_INVAL;
    struct { int64_t tv_sec; int64_t tv_nsec; } ts;
    memcpy(&ts, (const void *)(uintptr_t)utmo, sizeof(ts));
    if (ts.tv_sec == 0 && ts.tv_nsec == 0)
        *ticks = 0;
    else
        *ticks = (int64_t)ts.tv_sec * 100 + (int64_t)ts.tv_nsec / 10000000;
    return 0;
}

static int64_t sys_ppoll(uint64_t ufds, uint64_t nfds, uint64_t utmo, uint64_t sigset)
{
    (void)sigset;
    if (nfds > 4096)
        nfds = 4096;
    if (nfds == 0)
        return 0;
    if (!user_ptr_ok(ufds, nfds * 8))
        return -E_INVAL;

    int64_t ticks;
    int e = ppoll_timeout(utmo, &ticks);
    if (e < 0)
        return e;

    /* The buffer stays user memory; do_ppoll reads it via the direct mapping. */
    return do_ppoll((uint8_t *)(uintptr_t)ufds, nfds, ticks);
}

/* poll(7) is ppoll(271) with a millisecond timeout instead of a timespec.
 * -1 means infinite, otherwise it is rounded up to one tick. */
static int64_t sys_poll(uint64_t ufds, uint64_t nfds, int64_t ms)
{
    int64_t ticks;
    if (ms < 0)
        ticks = -1;
    else if (ms == 0)
        ticks = 0;
    else
        ticks = (ms + 9) / 10;        /* rounding up to one tick (100 Hz) */
    return do_ppoll((uint8_t *)(uintptr_t)ufds, nfds, ticks);
}

/*
 * select(23) -- the call BusyBox nc/wget actually make.  musl's select() is a
 * real syscall on x86-64 (not multiplexed), and it takes three fd_sets (read,
 * write, except) each 128 bytes of 1024 bits, plus a struct timeval
 * {long tv_sec; long tv_usec}.
 *
 * We translate the fd_sets to a pollfd array (in kernel scratch), run the same
 * scan/block core as poll, then write the ready bits back in place.  exceptfds
 * are never generated by anything we report, so they come back empty -- which
 * is exactly what these tools expect.
 */
typedef uint64_t fdset_t[16];          /* 1024 bits, the layout musl uses */

static inline int  fdset_get(const fdset_t *s, int fd)
{
    if (fd < 0 || fd >= 1024) return 0;
    return (int)(((*s)[fd / 64] >> (fd % 64)) & 1u);
}
static inline void fdset_set(fdset_t *s, int fd)
{
    if (fd < 0 || fd >= 1024) return;
    (*s)[fd / 64] |= (1ull << (fd % 64));
}

/*
 * Shared body for select(23) and pselect6(270).  Both take the same classic
 * fd_set bitmaps (read/write/except) -- 128 bytes of 1024 bits, the layout
 * musl's fd_set uses -- and differ only in timeout encoding.  exceptfds are
 * never set: nothing this kernel reports counts as an exceptional condition.
 */
static int64_t select_common(int nfds, uint64_t ur, uint64_t uw, uint64_t ue,
                             int64_t ticks)
{
    if (nfds < 0)
        return -E_INVAL;
    if (nfds > PROC_MAX_FD)
        nfds = PROC_MAX_FD;

    /* Read the caller's masks once; select writes the result back in place. */
    fdset_t in_r = {0}, in_w = {0};
    if (ur && !user_ptr_ok(ur, sizeof(fdset_t))) return -E_FAULT;
    if (uw && !user_ptr_ok(uw, sizeof(fdset_t))) return -E_FAULT;
    if (ue && !user_ptr_ok(ue, sizeof(fdset_t))) return -E_FAULT;
    if (ur) memcpy(&in_r, (const void *)(uintptr_t)ur, sizeof(in_r));
    if (uw) memcpy(&in_w, (const void *)(uintptr_t)uw, sizeof(in_w));

    /* Build a pollfd array in kernel scratch. */
    uint8_t pbuf[PROC_MAX_FD * 8];
    for (int i = 0; i < nfds; i++) {
        uint16_t ev = 0;
        if (fdset_get(&in_r, i)) ev |= POLLIN;
        if (fdset_get(&in_w, i)) ev |= POLLOUT;
        *(int32_t *)(pbuf + i*8 + 0) = i;
        *(int16_t *)(pbuf + i*8 + 4) = (int16_t)ev;
        *(int16_t *)(pbuf + i*8 + 6) = 0;
    }

    int64_t r = do_ppoll(pbuf, (uint64_t)nfds, ticks);
    if (r < 0)
        return r;

    /* Clear the user masks and set the bits for descriptors that are ready. */
    fdset_t out_r = {0}, out_w = {0};
    for (int i = 0; i < nfds; i++) {
        int16_t rev = *(int16_t *)(pbuf + i*8 + 6);
        if (rev & POLLIN)  fdset_set(&out_r, i);
        if (rev & POLLOUT) fdset_set(&out_w, i);
    }
    if (ur) memcpy((void *)(uintptr_t)ur, &out_r, sizeof(out_r));
    if (uw) memcpy((void *)(uintptr_t)uw, &out_w, sizeof(out_w));
    (void)ue;
    return r;
}

static int64_t sys_select(int nfds, uint64_t ur, uint64_t uw, uint64_t ue,
                          uint64_t utv)
{
    /* Translate the timeval (NULL => block forever) into timer ticks. */
    int64_t ticks = -1;
    if (utv) {
        struct { int64_t tv_sec; int64_t tv_usec; } tv;
        if (!user_ptr_ok(utv, sizeof(tv)))
            return -E_FAULT;
        memcpy(&tv, (const void *)(uintptr_t)utv, sizeof(tv));
        if (tv.tv_sec == 0 && tv.tv_usec == 0)
            ticks = 0;
        else
            ticks = (int64_t)tv.tv_sec * 100 + (int64_t)tv.tv_usec / 10000;
    }
    return select_common(nfds, ur, uw, ue, ticks);
}

/*
 * pselect6(270): select with a timespec timeout and a (here ignored) sigmask.
 * The sigmask would have to be swapped in atomically around the wait, which
 * this single-signal-mask-per-process kernel has no machinery for; ignoring
 * it is harmless because no caller here relies on the race it closes.
 */
static int64_t sys_pselect6(int nfds, uint64_t ur, uint64_t uw, uint64_t ue,
                            uint64_t uts, uint64_t usig)
{
    (void)usig;
    int64_t ticks = -1;
    if (uts) {
        struct { int64_t tv_sec; int64_t tv_nsec; } ts;
        if (!user_ptr_ok(uts, sizeof(ts)))
            return -E_FAULT;
        memcpy(&ts, (const void *)(uintptr_t)uts, sizeof(ts));
        if (ts.tv_sec == 0 && ts.tv_nsec == 0)
            ticks = 0;
        else
            ticks = (int64_t)ts.tv_sec * 100 + (int64_t)ts.tv_nsec / 10000000;
    }
    return select_common(nfds, ur, uw, ue, ticks);
}

/*
 * The private signal(2) our own libc uses.  It only ever says "ignore" or
 * "default" -- handlers go through rt_sigaction -- but it writes the same
 * disposition table, so the two entry points cannot end up disagreeing about
 * what a process wants.
 */
static int64_t sys_signal(int sig, int disposition)
{
    proc_t *p = proc_current();
    if (!p || sig <= 0 || sig >= NSIG)
        return -E_INVAL;
    /* The two signals a process is never allowed to escape. */
    if (sig == SIGKILL || sig == SIGSTOP)
        return -E_INVAL;

    p->sigact[sig].handler  = (disposition == SIG_IGN) ? SIG_IGN : SIG_DFL;
    p->sigact[sig].flags    = 0;
    p->sigact[sig].restorer = 0;
    p->sigact[sig].mask     = 0;

    if (disposition == SIG_IGN)
        p->sig_ignored |= SIGMASK(sig);
    else
        p->sig_ignored &= ~SIGMASK(sig);
    return 0;
}

/* ---- process groups --------------------------------------------------- */
static int64_t sys_setpgid(int pid, int pgid)
{
    proc_t *me = proc_current();
    if (!me)
        return -E_SRCH;

    proc_t *t = pid ? proc_by_pid(pid) : me;
    if (!t)
        return -E_SRCH;

    t->pgid = pgid ? pgid : t->pid;
    return 0;
}

static int64_t sys_getpgid(int pid)
{
    proc_t *me = proc_current();
    if (!me)
        return -E_SRCH;
    proc_t *t = pid ? proc_by_pid(pid) : me;
    if (!t)
        return -E_SRCH;
    return t->pgid;
}

/* ---- ioctl(16) ---------------------------------------------------------
 *
 * Every terminal operation user space has -- tcgetattr, tcsetattr, isatty,
 * tcgetpgrp, tcsetpgrp, window size -- is an ioctl on a descriptor, using the
 * request numbers Linux uses, because that is literally what musl's wrappers
 * issue.  Nothing else in the system has ioctls, so the whole call is "is
 * this fd the tty?  then ask the tty", and ENOTTY for everything else.
 *
 * ENOTTY (rather than a blanket 0) matters: isatty() is only an ioctl probe,
 * so answering "yes" for every descriptor would convince libc that a pipe or
 * a file is a terminal and send it down line-buffering and job-control paths
 * that do not apply.
 */
static int64_t sys_ioctl(int fd, uint64_t cmd, uint64_t arg)
{
    int h = fd_handle(fd);
    if (h < 0)
        return -E_BADF;
    /* A socket fd carries the SIOC* family of ioctls ifconfig/route use. */
    if (vfs_file_kind(h) == VFS_SOCKET)
        return net_if_ioctl(cmd, arg);
    /* A character device with its own ioctl handler (framebuffer, ...) gets
     * first crack; it returns -E_NOTTY for requests it does not recognise so
     * the generic "not a terminal" answer still flows through. */
    const vfs_ops_t *ops = vfs_file_ops(h);
    if (vfs_file_kind(h) == VFS_CHARDEV && ops && ops->ioctl) {
        const vfs_node_t *n = vfs_file_node(h);
        int64_t r = ops->ioctl((vfs_node_t *)n, cmd, arg);
        if (r != -E_NOTTY)
            return r;
    }
    if (ops != (const vfs_ops_t *)tty_ops())
        return -E_NOTTY;

    switch (cmd) {
    case TCGETS: {
        if (!user_ptr_ok(arg, sizeof(termios_t)))
            return -E_INVAL;
        tty_get_termios((termios_t *)(uintptr_t)arg);
        return 0;
    }

    /* TCSETS / TCSETSW / TCSETSF differ only in what happens to traffic
     * still in flight.  Output is never queued here, so "drain" is a no-op
     * and only TCSAFLUSH's discard of unread input has any effect. */
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        if (!user_ptr_ok(arg, sizeof(termios_t)))
            return -E_INVAL;
        if (tty_check_ttou())
            return -E_INTR;
        tty_set_termios((const termios_t *)(uintptr_t)arg, cmd == TCSETSF);
        return 0;
    }

    case TIOCGWINSZ: {
        if (!user_ptr_ok(arg, sizeof(winsize_t)))
            return -E_INVAL;
        tty_get_winsize((winsize_t *)(uintptr_t)arg);
        return 0;
    }

    case TIOCSWINSZ:
        /* The console is exactly as big as the framebuffer makes it. */
        return 0;

    case TIOCGPGRP: {
        if (!user_ptr_ok(arg, sizeof(int)))
            return -E_INVAL;
        *(int *)(uintptr_t)arg = tty_get_pgrp();
        return 0;
    }

    case TIOCSPGRP: {
        if (!user_ptr_ok(arg, sizeof(int)))
            return -E_INVAL;
        if (tty_check_ttou())
            return -E_INTR;
        /* Handing the terminal over is the act that makes a job the
         * foreground job; everything else about fg/bg follows from it. */
        tty_set_pgrp(*(const int *)(uintptr_t)arg);
        return 0;
    }

    case TIOCSCTTY:
        /* There is one terminal and every process shares it, so acquiring it
         * as a controlling tty is already true by the time you ask. */
        return 0;
    }

    return -E_INVAL;
}

/*
 * ttyinject(404) -- a headless-test back door, not POSIX.  It pushes len bytes
 * from a user buffer straight into the line discipline as though the keyboard
 * IRQ had produced them, so readlinetest.c can drive the terminal with no
 * human at it.  The length is capped so a wild call cannot pin the kernel
 * copying unbounded user memory.
 */
#define TTYINJECT_MAX 4096
static int64_t sys_ttyinject(uint64_t ubuf, uint64_t len)
{
    if (!ubuf || len == 0)
        return 0;
    if (len > TTYINJECT_MAX)
        len = TTYINJECT_MAX;
    if (!user_ptr_ok(ubuf, len))
        return -E_FAULT;

    char scratch[TTYINJECT_MAX];
    memcpy(scratch, (const void *)(uintptr_t)ubuf, len);
    tty_inject(scratch, (uint32_t)len);
    return (int64_t)len;
}

/* ---- memory management (mmap/brk) --------------------------------------
 * musl's malloc is built almost entirely on these three calls, so they are
 * the backbone of getting any libc program to run.
 *
 * Anonymous mappings live in a bump arena well below the stack; the arena
 * grows upward and each mapping is remembered (base + size) so munmap() and
 * exit can release it.  Eight slots is enough for a teaching OS and keeps the
 * bookkeeping in the PCB with no allocator.
 */
#define MMAP_BASE  USER_MMAP_BASE
#define MMAP_TOP   USER_MMAP_CEIL           /* never climb into the brk heap */

static int mmap_record(proc_t *p, uint64_t base, uint64_t size)
{
    if (p->nmmaps >= (int)(sizeof(p->mmaps) / sizeof(p->mmaps[0])))
        return 0;
    p->mmaps[p->nmmaps].base = base;
    p->mmaps[p->nmmaps].size = size;
    p->nmmaps++;
    return 1;
}

static int mmap_find(proc_t *p, uint64_t addr, uint64_t *base, uint64_t *size)
{
    for (int i = 0; i < p->nmmaps; i++) {
        if (p->mmaps[i].base <= addr && addr < p->mmaps[i].base + p->mmaps[i].size) {
            *base = p->mmaps[i].base;
            *size = p->mmaps[i].size;
            return i;
        }
    }
    return -1;
}

static void mmap_forget(proc_t *p, int idx)
{
    if (idx < 0)
        return;
    for (int i = idx; i < p->nmmaps - 1; i++)
        p->mmaps[i] = p->mmaps[i + 1];
    p->nmmaps--;
}

/* Choose a free user-virtual span at or above MMAP_BASE, below MMAP_TOP.
 * Returns 0 if no room.  MAP_FIXED / hinted addresses are handled by the
 * caller; this only serves the "let the kernel pick" case. */
static uint64_t mmap_pick_base(proc_t *p, uint64_t size)
{
    uint64_t base = MMAP_BASE;
    for (int i = 0; i < p->nmmaps; i++) {
        if (p->mmaps[i].base < MMAP_BASE || p->mmaps[i].base >= MMAP_TOP)
            continue;
        uint64_t top = p->mmaps[i].base + p->mmaps[i].size;
        if (top > base)
            base = top;
    }
    if (base + size > MMAP_TOP)
        return 0;
    return base;
}

static int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                        uint64_t flags, int64_t fd)
{
    proc_t *p = proc_current();
    if (!p || len == 0)
        return -E_INVAL;

    unsigned vflags = VM_USER;
    if (prot & PROT_WRITE) vflags |= VM_WRITE;
    if (prot & PROT_EXEC)  vflags |= VM_EXEC;

    uint64_t size = (len + PAGE_SIZE - 1) & ~0xFFFULL;

    /* A file/device mapping: today only character devices with an mmap op
     * (the framebuffer) are supported -- the device hands back the physical
     * range to map, so no page population from an fd is needed. */
    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
        int h = fd_handle((int)fd);
        if (h < 0)
            return -E_BADF;
        if (vfs_file_kind(h) != VFS_CHARDEV)
            return -E_NOSYS;
        const vfs_ops_t *ops = vfs_file_ops(h);
        if (!ops || !ops->mmap)
            return -E_NOSYS;
        uint64_t phys = 0, dsize = 0;
        if (ops->mmap((vfs_node_t *)vfs_file_node(h), &phys, &dsize) < 0)
            return -E_INVAL;
        uint64_t drounded = (dsize + PAGE_SIZE - 1) & ~0xFFFULL;
        if (size > drounded)
            size = drounded;

        uint64_t base = (flags & MAP_FIXED) ? (addr & ~0xFFFULL)
                      : (addr ? (addr & ~0xFFFULL) : mmap_pick_base(p, size));
        if (!base || base + size > USER_LIMIT)
            return -E_INVAL;
        /* Map the whole span, not just its first page: a 1024x768 framebuffer
         * is three megabytes and a single vmm_map() would leave user space
         * faulting one page in. */
        for (uint64_t o = 0; o < size; o += PAGE_SIZE) {
            if (!vmm_map(p->as, base + o, phys + o, vflags)) {
                vmm_unmap(p->as, base, o);
                return -ENOMEM;
            }
        }
        if (!mmap_record(p, base, size))
            return -ENOMEM;
        return (int64_t)base;
    }

    /* Only anonymous memory is otherwise backed here.  A file mapping would
     * need the pages populated from the fd, and quietly handing back zeroed
     * anonymous memory instead reads as an empty file rather than a failure. */
    if (!(flags & MAP_ANONYMOUS) || fd >= 0)
        return -E_NOSYS;

    uint64_t base;
    if (flags & MAP_FIXED) {
        /* The caller insists on this address; trust it (it is their mmaps
         * to manage) but keep it inside the user half.  musl claims its first
         * heap page this way at the brk base, which is outside the auto-arena
         * below, so we must not refuse it for being below MMAP_BASE. */
        if (addr == 0 || addr + size > USER_LIMIT)
            return -E_INVAL;
        base = addr;
    } else if (addr != 0) {
        base = addr & ~0xFFFULL;        /* honour a hint when given */
    } else {
        base = mmap_pick_base(p, size);
        if (!base)
            return -ENOMEM;
    }

    if (!vmm_alloc_range(p->as, base, size, vflags)) {
        return -ENOMEM;
    }
    if (!mmap_record(p, base, size))
        return -ENOMEM;          /* arena full: leak the pages, report */

    return (int64_t)base;
}

static int64_t sys_munmap(uint64_t addr, uint64_t len)
{
    proc_t *p = proc_current();
    (void)len;                        /* we only unmap whole mappings today */
    if (!p)
        return -E_INVAL;

    uint64_t base, size;
    int idx = mmap_find(p, addr, &base, &size);
    if (idx < 0)
        return -EINVAL;          /* not one of ours; Linux would EINVAL too */

    /* We only support unmapping a whole mapping, which is what libc's free
     * path hands us; partial unmaps are rare and not needed here yet. */
    if (addr != base)
        return -EINVAL;

    vmm_unmap(p->as, base, size);
    mmap_forget(p, idx);
    return 0;
}

static int64_t sys_brk(uint64_t addr)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;

    if (addr == 0)
        return (int64_t)p->brk;          /* brk(0) queries the current break */

    /* Refuse to move outside [base, ceil): below the base, or up into the mmap
     * arena / the stack.  Leaving the break unchanged reads as "no change". */
    if (addr < USER_BRK_BASE || addr > USER_BRK_CEIL)
        return (int64_t)p->brk;

    if (addr > p->brk) {
        if (!vmm_alloc_range(p->as, p->brk, addr - p->brk, VM_USER | VM_WRITE))
            return (int64_t)p->brk;
    } else if (addr < p->brk) {
        vmm_unmap(p->as, addr, p->brk - addr);
    }
    p->brk = addr;
    return (int64_t)p->brk;
}

/* ---- signals (sigaction / sigprocmask / sigreturn) --------------------- */
/*
 * The struct rt_sigaction passes is the *kernel's* 32-byte one, not the
 * 152-byte struct sigaction in <signal.h>; libc translates between them.  It
 * is laid out handler, flags, restorer, mask -- note that flags comes second,
 * which is not the order the user-space struct uses.
 */
typedef struct {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
} k_sigaction_t;

/* Neither of these can be caught, blocked or ignored, by anybody, ever. */
#define SIG_UNCATCHABLE  (SIGMASK(SIGKILL) | SIGMASK(SIGSTOP))

static int64_t sys_rt_sigaction(int sig, uint64_t uact, uint64_t uold,
                                uint64_t sigsetsize)
{
    proc_t *p = proc_current();
    if (!p || sig <= 0 || sig >= NSIG)
        return -E_INVAL;
    if (sig == SIGKILL || sig == SIGSTOP)
        return -E_INVAL;
    /* musl always passes 8; anything else means the two sides disagree about
     * how wide a sigset is, and quietly guessing would corrupt the mask. */
    if (sigsetsize != sizeof(uint64_t))
        return -E_INVAL;

    if (uold) {
        if (!user_ptr_ok(uold, sizeof(k_sigaction_t)))
            return -E_INVAL;
        k_sigaction_t *o = (k_sigaction_t *)(uintptr_t)uold;
        o->handler  = p->sigact[sig].handler;
        o->flags    = p->sigact[sig].flags;
        o->restorer = p->sigact[sig].restorer;
        o->mask     = p->sigact[sig].mask;
    }

    if (!uact)
        return 0;
    if (!user_ptr_ok(uact, sizeof(k_sigaction_t)))
        return -E_INVAL;

    const k_sigaction_t *a = (const k_sigaction_t *)(uintptr_t)uact;
    uint64_t handler = a->handler;

    /*
     * x86-64 has no kernel-owned return trampoline: the sequence that turns
     * "the handler returned" into rt_sigreturn() lives in libc and is named
     * by sa_restorer.  Without it we would have to map a page of our own into
     * every address space for the sake of nine bytes, so -EFAULT it is --
     * exactly what Linux answers.
     */
    if (handler != SIG_DFL && handler != SIG_IGN) {
        if (!(a->flags & SA_RESTORER) || !user_ptr_ok(a->restorer, 1) ||
            !user_ptr_ok(handler, 1))
            return -E_FAULT;
    }

    p->sigact[sig].handler  = handler;
    p->sigact[sig].flags    = a->flags;
    p->sigact[sig].restorer = a->restorer;
    p->sigact[sig].mask     = a->mask & ~SIG_UNCATCHABLE;

    if (handler == SIG_IGN) {
        p->sig_ignored |= SIGMASK(sig);
        /* Drop what was already queued: a signal sent while nobody was
         * listening must not surface later when the disposition changes. */
        p->sig_pending &= ~SIGMASK(sig);
    } else {
        p->sig_ignored &= ~SIGMASK(sig);
    }
    return 0;
}

static int64_t sys_rt_sigprocmask(int how, uint64_t uset, uint64_t uold,
                                  uint64_t sigsetsize)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;
    if (sigsetsize != sizeof(uint64_t))
        return -E_INVAL;

    if (uold) {
        if (!user_ptr_ok(uold, sizeof(uint64_t)))
            return -E_INVAL;
        *(uint64_t *)(uintptr_t)uold = p->sig_mask;
    }

    if (!uset)
        return 0;
    if (!user_ptr_ok(uset, sizeof(uint64_t)))
        return -E_INVAL;

    uint64_t set = *(const uint64_t *)(uintptr_t)uset;

    switch (how) {
    case SIG_BLOCK:   p->sig_mask |=  set; break;
    case SIG_UNBLOCK: p->sig_mask &= ~set; break;
    case SIG_SETMASK: p->sig_mask  =  set; break;
    default:          return -E_INVAL;
    }
    /* musl's fork() blocks everything with an all-ones set and restores the
     * old mask afterwards, so this filter runs on every fork in the system. */
    p->sig_mask &= ~SIG_UNCATCHABLE;
    return 0;
}

/* rt_sigpending(127): the set of signals currently pending for this process.
 * sig_pending already uses the (sig-1) bit convention musl expects, so it
 * copies straight across. */
static int64_t sys_rt_sigpending(uint64_t uset, uint64_t sigsetsize)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;
    if (sigsetsize != sizeof(uint64_t))
        return -E_INVAL;
    if (!user_ptr_ok(uset, sizeof(uint64_t)))
        return -E_INVAL;
    *(uint64_t *)(uintptr_t)uset = p->sig_pending;
    return 0;
}

/*
 * rt_sigsuspend(130): install a mask, sleep until a signal that mask lets
 * through arrives, then return EINTR with the caller's original mask back in
 * place -- but only *after* the handler has run.
 *
 * That last clause is the whole reason this cannot be written as
 * "sigprocmask; pause; sigprocmask": the window between the second-to-last
 * two is exactly where the awaited signal likes to arrive, and restoring the
 * mask before delivery would leave the handler blocked out of its own signal.
 * proc_t::sig_restore_mask carries the original mask forward to
 * signal_deliver(), which is where the swap actually becomes visible.
 *
 * Getting this wrong is not subtle: timeout(1) blocks SIGALRM and then waits
 * here for it, so an ENOSYS (or a mask restored one step early) turns
 * `timeout 1 sleep 5` into an unkillable spin.
 */
static int64_t sys_rt_sigsuspend(uint64_t uset, uint64_t sigsetsize)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;
    if (sigsetsize != sizeof(uint64_t))
        return -E_INVAL;
    if (!user_ptr_ok(uset, sizeof(uint64_t)))
        return -E_FAULT;

    uint64_t newmask = *(const uint64_t *)(uintptr_t)uset & ~SIG_UNCATCHABLE;

    p->sig_saved_mask   = p->sig_mask;
    p->sig_restore_mask = 1;
    p->sig_mask         = newmask;

    /* One tick of granularity is plenty: the wake-up itself is not what the
     * caller is timing, and polling keeps this out of the business of every
     * wait queue in the kernel.  ITIMER_REAL is driven from the timer tick,
     * so a pending alarm shows up here within 10 ms of firing. */
    while (!proc_pending_signals(p))
        sched_block_timeout(WAIT_SLEEP, 1);

    /* Deliberately *not* restoring sig_mask here.  signal_deliver() consumes
     * sig_restore_mask on the way out to user mode and rt_sigreturn puts the
     * saved mask back once the handler returns.  The one case that leaves the
     * flag set is a pending signal whose default action terminates the
     * process, and a dying process does not care what its mask says. */
    return -E_INTR;
}

/* ---- futex ------------------------------------------------------------- */
/*
 * Real futex semantics (park-on-wait, wake-on-release) need a queue we do not
 * have, but for a single thread the operations can be no-ops: a WAIT that has
 * nothing to match returns immediately, and a WAKE wakes nobody.  libc's
 * fast paths take exactly this and make progress.
 */
static int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val)
{
    (void)uaddr; (void)val;
    switch (op & 0x7F) {
    case FUTEX_WAIT:
        return 0;               /* pretend the value never matched: move on */
    case FUTEX_WAKE:
        return 0;               /* nobody is parked to wake */
    default:
        return -ENOSYS;
    }
}

/* ---- TLS / tid / time -------------------------------------------------- */
static int64_t sys_arch_prctl(uint64_t code, uint64_t addr)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;

    switch (code) {
    case ARCH_SET_FS:
        p->fs_base = addr;
        proc_set_fs(p);                 /* take effect before we return */
        return 0;
    case ARCH_GET_FS:
        if (!user_ptr_ok(addr, 8))
            return -E_INVAL;
        *(uint64_t *)(uintptr_t)addr = p->fs_base;
        return 0;
    default:
        return -EINVAL;
    }
}

static int64_t sys_set_tid_address(uint64_t utid)
{
    proc_t *p = proc_current();
    if (!p)
        return -E_INVAL;
    p->clear_child_tid = utid;          /* zeroed in proc_exit() */
    return p->pid;
}

/*
 * Is this clock id one that counts from the epoch rather than from boot?
 * The distinction matters to `ls -l` and to make: a monotonic clock that
 * starts at zero puts every timestamp in 1970 and every file "in the
 * future".
 */
static int clock_is_realtime(uint64_t clkid)
{
    return clkid == CLOCK_REALTIME || clkid == CLOCK_REALTIME_COARSE;
}

static int64_t sys_clock_gettime(uint64_t clkid, uint64_t utp)
{
    if (clkid > CLOCK_BOOTTIME)
        return -E_INVAL;
    if (utp && user_ptr_ok(utp, sizeof(ktimespec_t))) {
        uint64_t ticks = timer_ticks();
        ktimespec_t *ts = (ktimespec_t *)(uintptr_t)utp;
        ts->tv_sec  = (int64_t)(ticks / SCHED_HZ);
        ts->tv_nsec = (int64_t)((ticks % SCHED_HZ) * (1000000000ULL / SCHED_HZ));
        if (clock_is_realtime(clkid))
            ts->tv_sec += (int64_t)timer_boot_epoch();
    }
    return 0;
}

/* clock_getres(229): the tick is the only resolution we have. */
static int64_t sys_clock_getres(uint64_t clkid, uint64_t utp)
{
    if (clkid > CLOCK_BOOTTIME)
        return -E_INVAL;
    if (utp && user_ptr_ok(utp, sizeof(ktimespec_t))) {
        ktimespec_t *ts = (ktimespec_t *)(uintptr_t)utp;
        ts->tv_sec  = 0;
        ts->tv_nsec = 1000000000L / SCHED_HZ;
    }
    return 0;
}

static int64_t sys_gettimeofday(uint64_t utv, uint64_t utz)
{
    (void)utz;                          /* timezones are ignored */
    if (utv && user_ptr_ok(utv, sizeof(ktimeval_t))) {
        uint64_t ticks = timer_ticks();
        ktimeval_t *tv = (ktimeval_t *)(uintptr_t)utv;
        tv->tv_sec  = (int64_t)(ticks / SCHED_HZ + timer_boot_epoch());
        tv->tv_usec = (int64_t)((ticks % SCHED_HZ) * (1000000ULL / SCHED_HZ));
    }
    return 0;
}

/* time(201): seconds since the epoch, optionally stored through a pointer. */
static int64_t sys_time(uint64_t utp)
{
    int64_t now = (int64_t)(timer_ticks() / SCHED_HZ + timer_boot_epoch());
    if (utp) {
        if (!user_ptr_ok(utp, 8))
            return -E_FAULT;
        *(int64_t *)(uintptr_t)utp = now;
    }
    return now;
}

/* ---- process accounting ------------------------------------------------
 *
 * Everything below is derived from the one quantity the kernel actually
 * measures: elapsed ticks.  There is no per-process CPU accounting, so a
 * process's "user time" is reported as its whole lifetime and its "system
 * time" as zero.  That is not true, but it is monotonic and non-decreasing,
 * which is the property every caller actually relies on -- bash's `time`
 * builtin samples times() twice and prints the difference, and a difference
 * that could go backwards is what would make it print nonsense.
 */
static uint64_t proc_elapsed_ticks(const proc_t *p)
{
    uint64_t now = timer_ticks();
    return (p && now > p->start_tick) ? now - p->start_tick : 0;
}

static int64_t sys_times(uint64_t ubuf)
{
    proc_t *p = proc_current();
    uint64_t elapsed = proc_elapsed_ticks(p);

    if (ubuf) {
        if (!user_ptr_ok(ubuf, sizeof(tms_t)))
            return -E_FAULT;
        tms_t *t = (tms_t *)(uintptr_t)ubuf;
        t->tms_utime  = (int64_t)elapsed;
        t->tms_stime  = 0;
        t->tms_cutime = 0;
        t->tms_cstime = 0;
    }
    /* The return value is ticks since an arbitrary point in the past; the
     * boot tick is as good a point as any, and it can never go backwards. */
    return (int64_t)timer_ticks();
}

static int64_t sys_getrusage(int who, uint64_t ubuf)
{
    if (!ubuf)
        return 0;
    if (!user_ptr_ok(ubuf, sizeof(rusage_t)))
        return -E_FAULT;

    rusage_t *ru = (rusage_t *)(uintptr_t)ubuf;
    memset(ru, 0, sizeof(*ru));

    /* Children are not accounted for separately, so RUSAGE_CHILDREN is an
     * honest zero rather than a wrong number. */
    if (who == RUSAGE_SELF)
        ticks_to_tv(proc_elapsed_ticks(proc_current()), &ru->ru_utime);
    return 0;
}

/*
 * getrlimit(97)/prlimit64(302).  The limits are fixed by how the kernel is
 * built, so there is nothing to store: report the real ceilings and accept
 * (but ignore) attempts to lower them.  bash reads RLIMIT_NOFILE at startup
 * to size its own descriptor bookkeeping, and its `ulimit` builtin prints
 * whatever comes back here.
 */
static void rlimit_for(int res, rlimit_t *out)
{
    switch (res) {
    case RLIMIT_NOFILE: out->rlim_cur = PROC_MAX_FD;      break;
    case RLIMIT_STACK:  out->rlim_cur = USER_STACK_SIZE;  break;
    case RLIMIT_NPROC:  out->rlim_cur = MAX_PROCS;        break;
    case RLIMIT_CORE:   out->rlim_cur = 0;                break;  /* no dumps */
    default:            out->rlim_cur = RLIM_INFINITY;    break;
    }
    out->rlim_max = out->rlim_cur;
}

static int64_t sys_getrlimit(int res, uint64_t ulim)
{
    if (res < 0 || res >= RLIMIT_NLIMITS)
        return -E_INVAL;
    if (!user_ptr_ok(ulim, sizeof(rlimit_t)))
        return -E_FAULT;
    rlimit_for(res, (rlimit_t *)(uintptr_t)ulim);
    return 0;
}

static int64_t sys_prlimit64(int pid, int res, uint64_t unew, uint64_t uold)
{
    proc_t *p = proc_current();
    if (pid != 0 && (!p || pid != p->pid))
        return -E_SRCH;                 /* only ourselves */
    if (res < 0 || res >= RLIMIT_NLIMITS)
        return -E_INVAL;

    if (uold) {
        if (!user_ptr_ok(uold, sizeof(rlimit_t)))
            return -E_FAULT;
        rlimit_for(res, (rlimit_t *)(uintptr_t)uold);
    }
    if (unew) {
        /* Validate it so a bad pointer is still an error, then drop it: the
         * limits are structural and cannot actually be changed. */
        if (!user_ptr_ok(unew, sizeof(rlimit_t)))
            return -E_FAULT;
    }
    return 0;
}

/* ---- sleeping ----------------------------------------------------------
 *
 * The sleep is rounded *up* to a whole tick: a program that asks for one
 * microsecond and gets zero ticks would spin, and `sleep 0.001` in a shell
 * loop is a real thing people write.
 */
static int64_t do_nanosleep(const ktimespec_t *req, uint64_t urem)
{
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L)
        return -E_INVAL;

    uint64_t ns    = (uint64_t)req->tv_nsec;
    uint64_t ticks = (uint64_t)req->tv_sec * SCHED_HZ +
                     (ns + (1000000000ULL / SCHED_HZ) - 1) /
                     (1000000000ULL / SCHED_HZ);
    if (!ticks)
        return 0;

    uint64_t deadline = timer_ticks() + ticks;
    proc_t  *p        = proc_current();

    while (timer_ticks() < deadline) {
        if (p && proc_pending_signals(p)) {
            /* POSIX wants the shortfall reported so a caller can resume the
             * sleep after handling the signal. */
            if (urem && user_ptr_ok(urem, sizeof(ktimespec_t))) {
                uint64_t left = deadline - timer_ticks();
                ktimespec_t *rem = (ktimespec_t *)(uintptr_t)urem;
                rem->tv_sec  = (int64_t)(left / SCHED_HZ);
                rem->tv_nsec = (int64_t)((left % SCHED_HZ) *
                                         (1000000000ULL / SCHED_HZ));
            }
            return -E_INTR;
        }
        sched_block_timeout(WAIT_SLEEP, deadline - timer_ticks());
    }

    if (urem && user_ptr_ok(urem, sizeof(ktimespec_t)))
        memset((void *)(uintptr_t)urem, 0, sizeof(ktimespec_t));
    return 0;
}

static int64_t sys_nanosleep(uint64_t ureq, uint64_t urem)
{
    if (!user_ptr_ok(ureq, sizeof(ktimespec_t)))
        return -E_FAULT;
    return do_nanosleep((const ktimespec_t *)(uintptr_t)ureq, urem);
}

/*
 * clock_nanosleep(230).  TIMER_ABSTIME (flag 1) asks to sleep *until* a
 * point in time rather than for a duration, which is what a program doing
 * periodic work uses to avoid drifting.
 */
#define TIMER_ABSTIME 1

static int64_t sys_clock_nanosleep(uint64_t clkid, int flags, uint64_t ureq,
                                   uint64_t urem)
{
    if (clkid > CLOCK_BOOTTIME)
        return -E_INVAL;
    if (!user_ptr_ok(ureq, sizeof(ktimespec_t)))
        return -E_FAULT;

    const ktimespec_t *req = (const ktimespec_t *)(uintptr_t)ureq;
    if (!(flags & TIMER_ABSTIME))
        return do_nanosleep(req, urem);

    /* Convert the absolute deadline into the relative form we can sleep on,
     * on the same time base the caller named. */
    int64_t base = (int64_t)(timer_ticks() / SCHED_HZ);
    if (clock_is_realtime(clkid))
        base += (int64_t)timer_boot_epoch();

    ktimespec_t rel = { req->tv_sec - base, req->tv_nsec };
    while (rel.tv_nsec < 0) { rel.tv_nsec += 1000000000L; rel.tv_sec--; }
    if (rel.tv_sec < 0)
        return 0;                       /* the deadline is already past */
    /* An absolute sleep has no meaningful remainder. */
    return do_nanosleep(&rel, 0);
}

/* ---- scatter/gather I/O ------------------------------------------------
 * musl's stdio pushes every buffer flush through writev and every refill
 * through readv, so without these two a musl program cannot print a single
 * character.  We walk the vector entry by entry and stop as soon as one
 * transfer comes up short, which is the behaviour stdio is written against.
 */
typedef struct { uint64_t base, len; } iovec_t;

static int64_t sys_rw_vec(int fd, uint64_t uiov, uint64_t cnt, int writing)
{
    if (cnt > 1024 || !user_ptr_ok(uiov, cnt * sizeof(iovec_t)))
        return -E_INVAL;

    const iovec_t *iov = (const iovec_t *)(uintptr_t)uiov;
    int h = fd_handle(fd);
    int64_t total = 0;

    for (uint64_t i = 0; i < cnt; i++) {
        uint32_t len = (uint32_t)iov[i].len;
        if (!len)
            continue;
        if (!user_ptr_ok(iov[i].base, len))
            return total ? total : -E_INVAL;

        int64_t n = writing
            ? vfs_file_write(h, (const void *)(uintptr_t)iov[i].base, len)
            : vfs_file_read(h, (void *)(uintptr_t)iov[i].base, len);

        if (n < 0)
            return total ? total : n;
        total += n;
        if ((uint32_t)n < len)
            break;
    }
    return total;
}

/* ---- dispatch --------------------------------------------------------- */
/* One bit per syscall number, set the first time that number is refused.  See
 * the default case at the bottom of syscall_handler(). */
static uint64_t g_nosys_logged[8];

void syscall_handler(regs_t *r)
{
    uint64_t nr = r->rax;
    uint64_t a1 = r->rdi;
    uint64_t a2 = r->rsi;
    uint64_t a3 = r->rdx;

    proc_t  *p  = proc_current();
    int64_t  ret;

    /* Remembered for SA_RESTART: by the time signals are looked at, RAX holds
     * the result rather than the call number. */
    if (p)
        p->syscall_nr = (int64_t)nr;

    switch (nr) {
    case SYS_read:
        if (!user_ptr_ok(a2, a3)) { ret = -E_INVAL; break; }
        ret = vfs_file_read(fd_handle((int)a1), (void *)(uintptr_t)a2,
                            (uint32_t)a3);
        break;

    case SYS_write:
        if (!user_ptr_ok(a2, a3)) { ret = -E_INVAL; break; }
        ret = vfs_file_write(fd_handle((int)a1), (const void *)(uintptr_t)a2,
                             (uint32_t)a3);
        break;

    /* pread64(17)/pwrite64(18): musl's pread/pwrite, and what any program
     * that does random access without an lseek dance -- an ELF loader, a
     * framebuffer client, sqlite -- reaches for first. */
    case SYS_pread64:
        if (!user_ptr_ok(a2, a3)) { ret = -E_INVAL; break; }
        if ((int64_t)r->r10 < 0)  { ret = -E_INVAL; break; }
        ret = vfs_file_pread(fd_handle((int)a1), (void *)(uintptr_t)a2,
                             (uint32_t)a3, r->r10);
        break;

    case SYS_pwrite64:
        if (!user_ptr_ok(a2, a3)) { ret = -E_INVAL; break; }
        if ((int64_t)r->r10 < 0)  { ret = -E_INVAL; break; }
        ret = vfs_file_pwrite(fd_handle((int)a1), (const void *)(uintptr_t)a2,
                              (uint32_t)a3, r->r10);
        break;

    case SYS_readv:
        ret = sys_rw_vec((int)a1, a2, a3, 0);
        break;

    case SYS_writev:
        ret = sys_rw_vec((int)a1, a2, a3, 1);
        break;

    case SYS_open:
        ret = sys_open(a1, (int)a2);
        break;

    case SYS_openat:
        ret = sys_openat((int)a1, a2, (int)a3);
        break;

    case SYS_fcntl:
        ret = sys_fcntl((int)a1, (int)a2, a3);
        break;

    case SYS_close:
        ret = sys_close((int)a1);
        break;

    case SYS_dup:
        ret = sys_dup((int)a1);
        break;

    case SYS_dup2:
        ret = sys_dup2((int)a1, (int)a2);
        break;

    case SYS_stat:
        ret = sys_stat(a1, a2, 1);
        break;

    case SYS_lstat:
        ret = sys_stat(a1, a2, 0);
        break;

    case SYS_fstat:
        ret = sys_fstat((int)a1, a2);
        break;

    case SYS_newfstatat:
        ret = sys_newfstatat((int)a1, a2, a3, r->r10);
        break;

    case SYS_getdents64:
        ret = sys_getdents64((int)a1, a2, a3);
        break;

    case SYS_gstat:
        ret = sys_gstat(a1, a2);
        break;

    case SYS_pipe:
        ret = sys_pipe(a1);
        break;

    case SYS_mkdir: {
        char abs[GNUOS_PATH_MAX];
        ret = path_abs(a1, abs);
        if (ret < 0) break;
        ret = vfs_mkdir(abs);
        break;
    }

    case SYS_unlink: {
        char abs[GNUOS_PATH_MAX];
        ret = path_abs(a1, abs);
        if (ret < 0) break;
        ret = vfs_unlink(abs);
        break;
    }

    case SYS_rmdir: {
        char abs[GNUOS_PATH_MAX];
        ret = path_abs(a1, abs);
        if (ret < 0) break;
        ret = vfs_rmdir(abs);
        break;
    }

    case SYS_chdir:
        ret = sys_chdir(a1);
        break;

    case SYS_fchdir:
        ret = sys_fchdir((int)a1);
        break;

    case SYS_getcwd:
        ret = sys_getcwd(a1, a2);
        break;

    case SYS_rename:
        ret = vfs_rename((const char *)a1, (const char *)a2);
        break;

    case SYS_truncate: {
        char abs[GNUOS_PATH_MAX];
        ret = path_abs(a1, abs);
        if (ret < 0) break;
        ret = (int64_t)a2 < 0 ? -E_INVAL : vfs_truncate(abs, a2);
        break;
    }

    case SYS_ftruncate: {
        /* Resolved through the descriptor's remembered path rather than its
         * node: the open file caches a size that the truncate is about to
         * invalidate, and going back through the VFS is what keeps the two
         * from disagreeing. */
        const char *p = vfs_file_path(fd_handle((int)a1));
        if (!p) { ret = -E_BADF; break; }
        ret = (int64_t)a2 < 0 ? -E_INVAL : vfs_truncate(p, a2);
        break;
    }

    case SYS_uname:
        ret = sys_uname(a1);
        break;

    case SYS_gethostname:
        ret = sys_gethostname(a1, a2);
        break;

    case SYS_sethostname:
        ret = sys_sethostname(a1, a2);
        break;

    case SYS_setitimer:
        ret = sys_setitimer(a1, a2, a3);
        break;

    case SYS_getitimer:
        ret = sys_getitimer(a1, a2);
        break;

    case SYS_alarm:
        ret = sys_alarm(a1);
        break;

    case SYS_umask:
        ret = sys_umask((uint32_t)a1);
        break;

    case SYS_getrandom:
        ret = sys_getrandom(a1, a2, a3);
        break;

    case SYS_mount:
        ret = sys_mount(a1, a2, a3, r->r10, r->r8);
        break;

    case SYS_umount2:
        ret = sys_umount2(a1, a2);
        break;

    case SYS_chmod:
        ret = sys_chmod(a1, (uint32_t)a2);
        break;

    case SYS_access:
        ret = sys_access(AT_FDCWD, a1, 0);
        break;

    case SYS_faccessat:
        ret = sys_access((int)a1, a2, 1);
        break;

    /* faccessat2(439): musl's faccessat() on a kernel that advertises it. */
    case SYS_faccessat2:
        ret = sys_access((int)a1, a2, 1);
        break;

    /* chown(92) and friends: every file already belongs to uid 0 and there is
     * no second user to hand it to, so the only ownership change anyone can
     * ask for is the one that is already in effect.  Say yes.  ENOSYS here
     * makes `mv` print "can't preserve ownership" after a move that did in
     * fact preserve everything that exists. */
    case SYS_chown:
    case SYS_fchown:
    case SYS_lchown:
        ret = 0;
        break;

    /* readlink(89): fill the caller's buffer with the link target.  POSIX
     * says the target is NOT nul-terminated and the result is its length. */
    case SYS_readlink:
        ret = sys_readlink((const char *)a1, a2, a3);
        break;

    /* ---- *at() namespace ops (the path the musl libc actually uses) ---- */
    /*
     * musl routes unlink/rmdir/mkdir/rename/symlink/readlink through their
     * *at() forms, so these are not optional extras -- without them nothing
     * can create or delete a file.  dirfd is AT_FDCWD for the cases we hit.
     */
    case SYS_unlinkat: {
        char abs[GNUOS_PATH_MAX];
        ret = path_at((int)a1, a2, abs);
        if (ret < 0) break;
        /* AT_REMOVEDIR (0x200) turns unlinkat into rmdir. */
        if (a3 & 0x200)
            ret = vfs_rmdir(abs);
        else
            ret = vfs_unlink(abs);
        break;
    }

    case SYS_mkdirat: {
        char abs[GNUOS_PATH_MAX];
        ret = path_at((int)a1, a2, abs);
        if (ret < 0) break;
        ret = vfs_mkdir(abs);
        break;
    }

    case SYS_renameat:
    case SYS_renameat2: {
        /* renameat(olddirfd, oldpath, newdirfd, newpath[, flags]):
         * newdirfd is the *third* argument (rdx = a3) and newpath the
         * fourth (r10) -- the same shape as readlinkat below.  Reading the
         * pair one slot along instead used newpath as a descriptor and the
         * flags word as a pointer, which page-faulted the kernel the first
         * time anything called renameat with a non-zero flags register.
         * musl's renameat() is a 4-argument syscall, so r8 is whatever the
         * caller happened to leave there: `mv` was enough to trip it. */
        char old[GNUOS_PATH_MAX], newp[GNUOS_PATH_MAX];
        ret = path_at((int)a1, a2, old);
        if (ret < 0) break;
        ret = path_at((int)a3, r->r10, newp);
        if (ret < 0) break;
        if (nr == SYS_renameat2) {
            uint32_t flags = (uint32_t)r->r8;
            /* RENAME_NOREPLACE is the one flag worth honouring: gnulib asks
             * for it whenever it must not clobber the destination, and
             * quietly ignoring it turns `mv -n` into `mv -f`.  EXCHANGE and
             * WHITEOUT need atomicity the ext2 driver cannot offer, so they
             * are refused rather than approximated. */
            if (flags & ~RENAME_NOREPLACE) {
                ret = -E_INVAL;
                break;
            }
            uint64_t sz; int kind;
            if ((flags & RENAME_NOREPLACE) && vfs_stat(newp, &sz, &kind) == 0) {
                ret = -E_EXIST;
                break;
            }
        }
        ret = vfs_rename(old, newp);
        break;
    }

    case SYS_symlink:
        ret = vfs_symlink((const char *)a1, (const char *)a2);
        break;

    case SYS_symlinkat: {
        char abs[GNUOS_PATH_MAX];
        ret = path_at((int)a2, a3, abs);
        if (ret < 0) break;
        ret = vfs_symlink((const char *)a1, abs);
        break;
    }

    case SYS_readlinkat: {
        char abs[GNUOS_PATH_MAX];
        ret = path_at((int)a1, a2, abs);
        if (ret < 0) break;
        /* readlinkat(dirfd, path, buf, bufsize): buf is a3 (RDX), bufsize
         * is the 4th argument r10 -- not r8. */
        ret = do_readlink(abs, a3, r->r10);
        break;
    }

    case SYS_fchmodat: {
        char abs[GNUOS_PATH_MAX];
        ret = path_at((int)a1, a2, abs);
        if (ret < 0) break;
        /* AT_SYMLINK_NOFOLLOW (0x100) would mean chmod the link itself, which
         * we cannot do; we always operate on the resolved target. */
        ret = vfs_chmod(abs, (uint32_t)a3);
        break;
    }

    case SYS_fchownat:
        /* Every file is root-owned and there is no second owner to hand it
         * to, so the only possible result is the one already in effect. */
        ret = 0;
        break;

    /* utimensat(280): we keep no timestamps, so there is nothing to set.
     * Report success rather than ENOSYS -- `cp -p` treats a failure here as
     * a warning worth printing, and a warning about a clock we do not have
     * is noise, not information. */
    case SYS_utimensat:
        ret = 0;
        break;

    case SYS_lseek:
        ret = vfs_file_seek(fd_handle((int)a1), (int64_t)a2, (int)a3);
        break;

    case SYS_ioctl:
        ret = sys_ioctl((int)a1, a2, a3);
        break;

    /* ttyinject(404): feed a buffer into the line discipline.  A debugging
     * back door for headless tests (see readlinetest.c); not POSIX. */
    case SYS_ttyinject:
        ret = sys_ttyinject(a1, a2);
        break;

    case SYS_sched_yield:
        sched_yield();
        ret = 0;
        break;

    case SYS_getpid:
        ret = p ? p->pid : 0;
        break;

    case SYS_getppid:
        ret = p ? p->ppid : 0;
        break;

    case SYS_fork:
        ret = proc_fork(r);
        break;

    case SYS_vfork:
        /* This teaching OS has no COW, so vfork behaves exactly like fork:
         * the child gets its own address space and must execve() or _exit().
         * That is what busybox ash needs vfork for (running external commands). */
        ret = proc_fork(r);
        break;

    case SYS_clone:
        /* musl routes both fork() and pthread_create through clone, so a
         * musl-linked userland (OpenRC, bash, ...) needs it even for a fork. */
        ret = proc_clone(r);
        break;

    case SYS_ppoll:
        ret = sys_ppoll(a1, a2, a3, r->r10);
        break;

    case SYS_poll:
        ret = sys_poll(a1, a2, (int64_t)a3);
        break;

    /* ---- BSD socket API (x86-64: one syscall per call, no socketcall) ---- */
    case SYS_socket:
        ret = sys_socket((int)a1, (int)a2, (int)a3);
        break;
    case SYS_bind:
        ret = sys_bind((int)a1, a2, a3);
        break;
    case SYS_connect:
        ret = sys_connect((int)a1, a2, a3);
        break;
    case SYS_listen:
        ret = sys_listen((int)a1, (int)a2);
        break;
    case SYS_accept:
        ret = sys_accept((int)a1, a2, a3, 0);
        break;
    case SYS_accept4:
        ret = sys_accept((int)a1, a2, a3, (int)r->r10);
        break;
    case SYS_sendto:
        /* arg order: fd, buf, len, flags, addr, addrlen */
        ret = sys_sendto((int)a1, a2, a3, (int)r->r10, r->r8, r->r9);
        break;
    case SYS_recvfrom:
        /* arg order: fd, buf, len, flags, addr, addrlen */
        ret = sys_recvfrom((int)a1, a2, a3, (int)r->r10, r->r8, r->r9);
        break;
    case SYS_shutdown:
        ret = sys_shutdown((int)a1, (int)a2);
        break;
    case SYS_getsockname:
        ret = sys_getsockname((int)a1, a2, a3, 0);
        break;
    case SYS_getpeername:
        ret = sys_getsockname((int)a1, a2, a3, 1);
        break;
    case SYS_setsockopt:
        ret = sys_setsockopt((int)a1, (int)a2, (int)a3, r->r10, r->r8);
        break;
    case SYS_getsockopt:
        ret = sys_getsockopt((int)a1, (int)a2, (int)a3, r->r10, r->r8);
        break;
    case SYS_select:
        /* int select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *xfds,
         *            struct timeval *tv);  args in RDI/RSI/RDX/R10/R8. */
        ret = sys_select((int)a1, a2, a3, r->r10, r->r8);
        break;

    case SYS_pselect6:
        /* pselect6 has the same fd_set layout; timeout is timespec and the
         * 6th arg (sigmask) is ignored -- see sys_pselect6(). */
        ret = sys_pselect6((int)a1, a2, a3, r->r10, r->r8, r->r9);
        break;

    case SYS_execve: {
        char abs[GNUOS_PATH_MAX];
        ret = path_abs(a1, abs);
        if (ret < 0) break;
        int n = collect_vec(a2, g_argv);
        if (n < 0) { ret = n; break; }
        n = collect_vec(a3, g_envp);
        if (n < 0) { ret = n; break; }
        ret = proc_execve(abs, g_argv, g_envp, r);
        if (ret == 0)
            return;                    /* the frame now belongs to the new image */
        break;
    }

    case SYS_wait4: {
        int status = 0;
        ret = proc_waitpid((int)a1, &status, (int)a3);
        if (ret > 0 && a2) {
            if (!user_ptr_ok(a2, sizeof(int))) { ret = -E_INVAL; break; }
            *(int *)(uintptr_t)a2 = status;
        }
        break;
    }

    case SYS_kill:
        ret = sys_kill((int)a1, (int)a2);
        break;

    case SYS_tkill:
        ret = sys_tkill((int)a1, (int)a2);
        break;

    case SYS_tgkill:
        ret = sys_tgkill((int)a1, (int)a2, (int)a3);
        break;

    case SYS_signal:
        ret = sys_signal((int)a1, (int)a2);
        break;

    case SYS_setpgid:
        ret = sys_setpgid((int)a1, (int)a2);
        break;

    case SYS_getpgid:
        ret = sys_getpgid((int)a1);
        break;

    /* ---- musl libc support -------------------------------------------- */
    case SYS_mmap:
        ret = sys_mmap(a1, a2, a3, r->r10, (int64_t)(int32_t)r->r8);
        break;

    case SYS_munmap:
        ret = sys_munmap(a1, a2);
        break;

    case SYS_brk:
        ret = sys_brk(a1);
        break;

    case SYS_mprotect:
        ret = 0;                       /* accept; permissions already set */
        break;

    case SYS_madvise:
        ret = 0;                       /* accepted and ignored */
        break;

    case SYS_rt_sigaction:
        ret = sys_rt_sigaction((int)a1, a2, a3, r->r10);
        break;

    case SYS_rt_sigprocmask:
        ret = sys_rt_sigprocmask((int)a1, a2, a3, r->r10);
        break;

    case SYS_rt_sigpending:
        ret = sys_rt_sigpending(a1, r->r10);
        break;

    case SYS_sigsuspend:
        ret = sys_rt_sigsuspend(a1, a2);
        break;

    case SYS_rt_sigreturn:
        /* Returns nothing: every register, RAX included, comes back from the
         * signal frame, so this must not fall through to the assignment at
         * the bottom of the function. */
        signal_return(r);
        return;

    case SYS_futex:
        ret = sys_futex(a1, a2, a3);
        break;

    case SYS_arch_prctl:
        ret = sys_arch_prctl(a1, a2);
        break;

    case SYS_set_tid_address:
        ret = sys_set_tid_address(a1);
        break;

    case SYS_gettid:
        ret = p ? p->pid : 0;
        break;

    case SYS_clock_gettime:
        ret = sys_clock_gettime(a1, a2);
        break;

    case SYS_clock_getres:
        ret = sys_clock_getres(a1, a2);
        break;

    case SYS_gettimeofday:
        ret = sys_gettimeofday(a1, a2);
        break;

    case SYS_time:
        ret = sys_time(a1);
        break;

    case SYS_times:
        ret = sys_times(a1);
        break;

    case SYS_getrusage:
        ret = sys_getrusage((int)a1, a2);
        break;

    case SYS_getrlimit:
        ret = sys_getrlimit((int)a1, a2);
        break;

    case SYS_setrlimit:
        /* The limits are structural, so there is nothing to set; a shell
         * that cannot lower one is better off than one that cannot start. */
        ret = 0;
        break;

    case SYS_prlimit64:
        ret = sys_prlimit64((int)a1, (int)a2, a3, r->r10);
        break;

    case SYS_nanosleep:
        ret = sys_nanosleep(a1, a2);
        break;

    case SYS_clock_nanosleep:
        ret = sys_clock_nanosleep(a1, (int)a2, a3, r->r10);
        break;

    case SYS_dup3:
        ret = sys_dup3((int)a1, (int)a2, (int)a3);
        break;

    case SYS_pipe2:
        ret = sys_pipe2(a1, (int)a2);
        break;

    case SYS_getuid:
    case SYS_geteuid:
    case SYS_getgid:
    case SYS_getegid:
        ret = 0;                       /* single root user */
        break;

    /*
     * getresuid(118)/getresgid(120): there is exactly one user (root, 0) and
     * no saved-set-uid dance, so every r/e/s uid/gid is 0.  Each argument may
     * be NULL, which we must not write through.
     */
    case SYS_getresuid:
        ret = sys_getresuid(a1, a2, a3);
        break;
    case SYS_getresgid:
        ret = sys_getresgid(a1, a2, a3);
        break;

    case SYS_exit_group:
        proc_exit((int)a1);
        ret = 0;                       /* not reached */
        break;

    case SYS_exit:
        proc_exit((int)a1);
        ret = 0;                       /* not reached */
        break;

    default:
        /* Once per syscall number, not once per call.  A libc that retries in
         * a loop on ENOSYS -- which is exactly what happens when a wait
         * primitive is missing -- otherwise turns the debug console into the
         * slowest part of the system: one boot with rt_sigsuspend missing
         * wrote six megabytes of this single line. */
        if (nr < 512 && !(g_nosys_logged[nr >> 6] & (1ULL << (nr & 63)))) {
            g_nosys_logged[nr >> 6] |= 1ULL << (nr & 63);
            dbg_puts("GNOS: unimplemented syscall ");
            dbg_puts_dec((uint32_t)nr);
            dbg_puts("\r\n");
        }
        ret = -E_NOSYS;
        break;
    }

    r->rax = (uint64_t)ret;
}

void syscall_init(void)
{
    syscall_install(syscall_handler);

    /* Enable the `syscall` instruction as a second entry point (musl uses
     * it instead of int 0x80).  We return to user mode with iretq, so only
     * the SYSCALL half of IA32_STAR matters: it supplies the kernel code
     * selector the CPU switches into on entry. */
    wrmsr(0xC0000081,                 /* IA32_STAR: syscall CS = kernel,  */
          ((uint64_t)SEL_KCODE << 32) /* sysret CS field (unused here)    */
          | ((uint64_t)SEL_UCODE << 48));
    wrmsr(0xC0000082, (uint64_t)syscall_entry);   /* IA32_LSTAR */
    wrmsr(0xC0000080, 0x00000801);    /* EFER.SCE = 1 | EFER.NXE = 1 */
    /* IA32_FMASK left at 0: the stub clears IF itself, and leaving RFLAGS
     * untouched means the saved R11 still carries the user's IF for iretq. */

    dbg_puts("GNOS: syscall gate installed (int 0x80 + syscall)\r\n");
}
