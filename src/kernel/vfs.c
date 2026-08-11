/*
 * vfs.c — one ext2 root mount, /dev character devices, and anonymous pipes.
 * (GPLv2)
 *
 * Three node kinds share one open-file table, and the differences between
 * them are worth naming because everything else here follows from them:
 *
 *   - A *file* has a position; read() and write() advance it, and a write
 *     past the end allocates more blocks underneath.
 *   - A *directory* is read as a stream of fixed-size gdirent_t records
 *     rather than raw bytes, so `ls` needs no parser and no allocator.
 *   - A *character device* and a *pipe* have no position at all: the offset
 *     is meaningless and is never advanced.
 *
 * Pipes are also the only nodes here that can block.  A reader sleeps until
 * there are bytes or every writer has gone away (that is end-of-file); a
 * writer sleeps until there is room or every reader has gone away (that is
 * SIGPIPE).  Both conditions are decided purely by the reference counts the
 * open-file table already maintains, which is why closing the write end of a
 * pipeline is what makes the reader finish.
 */
#include <stddef.h>
#include <stdint.h>

#include "vfs.h"
#include "ext2.h"
#include "proc.h"
#include "procfs.h"
#include "tmpfs.h"
#include "sock.h"
#include "kstring.h"
#include "debugcon.h"
#include "subsys.h"
#include "syscall.h"

typedef struct {
    int      used;
    int      readers;           /* references held on the read end  */
    int      writers;           /* references held on the write end */
    uint32_t head, tail, count;
    uint8_t  buf[VFS_PIPE_CAP];
} pipe_t;

typedef struct {
    int         refs;           /* 0 == this slot is free */
    vfs_node_t  node;
    uint64_t    pos;
    int         flags;
    char        path[VFS_PATH_MAX];   /* absolute path this descriptor names */
} vfs_file_t;

static ext2_fs_t  g_fs;
static int        g_fs_ok;

static vfs_node_t g_dev[VFS_MAX_DEV];
static unsigned   g_dev_count;

static vfs_file_t g_files[VFS_MAX_FILES];
static pipe_t     g_pipes[VFS_MAX_PIPES];

/* Map the ext2 driver's failure codes onto the errno the caller expects. */
static int fs_errno(int e)
{
    switch (e) {
    case EXT2_OK:        return 0;
    case EXT2_ENOENT:    return -E_NOENT;
    case EXT2_EEXIST:    return -E_EXIST;
    case EXT2_ENOSPC:    return -E_NOSPC;
    case EXT2_ENOTDIR:   return -E_NOTDIR;
    case EXT2_ENOTEMPTY: return -E_NOTEMPTY;
    case EXT2_EISDIR:    return -E_ISDIR;
    default:             return -E_INVAL;
    }
}

/* ---- the ext2 mount --------------------------------------------------- */
static int32_t fs_node_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    if (off > 0xFFFFFFFFULL)
        return 0;
    return (int32_t)ext2_read(&g_fs, &n->e2, (uint32_t)off, buf, len);
}

static int32_t fs_node_write(vfs_node_t *n, uint64_t off, const void *buf,
                             uint32_t len)
{
    if (off > 0xFFFFFFFFULL)
        return -E_INVAL;

    uint32_t w = ext2_write(&g_fs, &n->e2, (uint32_t)off, buf, len);
    if (!w)
        return len ? -E_NOSPC : 0;

    n->size = n->e2.size;
    return (int32_t)w;
}

static const vfs_ops_t g_fs_ops = { .read = fs_node_read, .write = fs_node_write };

/*
 * Reading a directory yields whole gdirent_t records.  The offset is a byte
 * count like any other, so lseek() and a plain sequence of read() calls both
 * work; we just divide it back into a record index.
 */
static int32_t dir_node_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    const uint32_t rec = (uint32_t)sizeof(gdirent_t);

    if (off % rec)
        return -E_INVAL;
    if (len < rec)
        return -E_INVAL;

    uint32_t skip = (uint32_t)(off / rec);
    uint32_t room = len / rec;

    gdirent_t    *out = (gdirent_t *)buf;
    ext2_dir_t    d;
    ext2_dirent_t e;

    ext2_opendir(&g_fs, n->e2.ino, &d);

    uint32_t seen = 0, got = 0;
    while (got < room && ext2_readdir(&d, &e)) {
        if (seen++ < skip)
            continue;

        int i = 0;
        while (i < GDIRENT_NAME - 1 && e.name[i]) {
            out[got].name[i] = e.name[i];
            i++;
        }
        while (i < GDIRENT_NAME)
            out[got].name[i++] = 0;

        out[got].size = e.size;
        out[got].kind = ext2_is_dir(&e) ? GK_DIR : GK_FILE;
        got++;
    }

    return (int32_t)(got * rec);
}

static int32_t dir_node_write(vfs_node_t *n, uint64_t off, const void *buf,
                              uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return -E_ISDIR;
}

static const vfs_ops_t g_dir_ops = { .read = dir_node_read, .write = dir_node_write };

/* ---- pipes ------------------------------------------------------------ */
static int32_t pipe_node_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)off;

    pipe_t  *p  = (pipe_t *)n->priv;
    proc_t  *me = proc_current();
    uint8_t *d  = (uint8_t *)buf;

    if (!p || !len)
        return 0;

    /* Test-and-sleep has to be atomic against the writer, and on a single
     * CPU with no kernel pre-emption "cli" is exactly that. */
    asm volatile("cli");
    while (p->count == 0) {
        if (p->writers == 0) {              /* nobody left to send anything */
            asm volatile("sti");
            return 0;
        }
        if (!me || proc_pending_signals(me)) {
            asm volatile("sti");
            return -E_INTR;
        }
        sched_block_irqoff(WAIT_PIPE);
    }

    uint32_t n2 = 0;
    while (n2 < len && p->count) {
        d[n2++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % VFS_PIPE_CAP;
        p->count--;
    }
    asm volatile("sti");

    sched_wake_reason(WAIT_PIPE);           /* a blocked writer has room now */
    return (int32_t)n2;
}

static int32_t pipe_node_write(vfs_node_t *n, uint64_t off, const void *buf,
                               uint32_t len)
{
    (void)off;

    pipe_t        *p    = (pipe_t *)n->priv;
    proc_t        *me   = proc_current();
    const uint8_t *s    = (const uint8_t *)buf;
    uint32_t       done = 0;

    if (!p || !len)
        return 0;

    while (done < len) {
        asm volatile("cli");

        while (p->count == VFS_PIPE_CAP && p->readers > 0) {
            if (!me || proc_pending_signals(me)) {
                asm volatile("sti");
                return done ? (int32_t)done : -E_INTR;
            }
            sched_block_irqoff(WAIT_PIPE);
        }

        if (p->readers == 0) {
            asm volatile("sti");
            /* Writing into a pipe with no reader is the classic broken
             * pipe; the default action of SIGPIPE ends the writer. */
            if (me)
                proc_signal(me, SIGPIPE);
            return done ? (int32_t)done : -E_PIPE;
        }

        while (done < len && p->count < VFS_PIPE_CAP) {
            p->buf[p->head] = s[done++];
            p->head = (p->head + 1) % VFS_PIPE_CAP;
            p->count++;
        }
        asm volatile("sti");

        sched_wake_reason(WAIT_PIPE);
    }

    return (int32_t)done;
}

/* ---- /dev/null --------------------------------------------------------- */
/* Reads return EOF, writes silently succeed.  No ioctl makes sense, so the
 * generic handler answers ENOTTY. */
static int32_t null_node_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return 0;
}

static int32_t null_node_write(vfs_node_t *n, uint64_t off, const void *buf,
                               uint32_t len)
{
    (void)n; (void)off; (void)buf;
    return (int32_t)len;
}

static const vfs_ops_t g_null_ops = { .read = null_node_read, .write = null_node_write };

/* ---- /dev/zero and /dev/full ------------------------------------------- */
/* Both read as an endless run of zero bytes.  They differ only in what a
 * write does, and that difference is the entire point of /dev/full: it is the
 * only portable way to make a program take its ENOSPC path on demand. */
static int32_t zero_node_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n; (void)off;
    memset(buf, 0, len);
    return (int32_t)len;
}

static int32_t full_node_write(vfs_node_t *n, uint64_t off, const void *buf,
                               uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return -E_NOSPC;
}

static const vfs_ops_t g_zero_ops = { .read = zero_node_read, .write = null_node_write };
static const vfs_ops_t g_full_ops = { .read = zero_node_read, .write = full_node_write };

/* ---- /dev/random and /dev/urandom -------------------------------------- */
/* One generator behind both names.  Linux stopped distinguishing them years
 * ago -- random(4) no longer blocks once seeded -- and a kernel with no
 * entropy source has nothing to gain by pretending otherwise: a blocking
 * /dev/random here would simply hang the boot. */
static int32_t random_node_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n; (void)off;
    krandom_bytes(buf, len);
    return (int32_t)len;
}

static const vfs_ops_t g_random_ops = { .read = random_node_read, .write = null_node_write };

static const vfs_ops_t g_pipe_ops = { .read = pipe_node_read, .write = pipe_node_write };

int vfs_init(uint8_t *img, uint32_t img_size)
{
    memset(&g_fs, 0, sizeof(g_fs));
    memset(g_dev, 0, sizeof(g_dev));
    memset(g_files, 0, sizeof(g_files));
    memset(g_pipes, 0, sizeof(g_pipes));
    g_dev_count = 0;

    g_fs_ok = ext2_mount(&g_fs, img, img_size);
    if (!g_fs_ok) {
        dbg_puts("VFS: initrd is not an ext2/ext3 volume\r\n");
        return 0;
    }

    dbg_puts("VFS: mounted ext2 read/write on / (");
    dbg_puts_dec(g_fs.blocks_count);
    dbg_puts(" blocks, ");
    dbg_puts_dec(g_fs.block_size);
    dbg_puts(" bytes/block, ");
    dbg_puts_dec(g_fs.inodes_count);
    dbg_puts(" inodes)\r\n");

    /* The memory devices.  These are the nodes a POSIX userland assumes are
     * simply there -- shells redirect to /dev/null, mke2fs and dd read
     * /dev/zero, and musl's ASLR and BusyBox's mktemp fall back to
     * /dev/urandom when getrandom(2) is unavailable.  They are registered
     * here rather than by a driver because they have no hardware behind
     * them: they are the VFS itself.  The major/minor numbers are Linux's,
     * so a coldplug run produces the same /dev a Linux box would. */
    static const struct {
        const char      *name;
        const vfs_ops_t *ops;
        uint16_t         minor;
    } memdevs[] = {
        { "null",    &g_null_ops,   3 },
        { "zero",    &g_zero_ops,   5 },
        { "full",    &g_full_ops,   7 },
        { "random",  &g_random_ops, 8 },
        { "urandom", &g_random_ops, 9 },
    };
    for (unsigned i = 0; i < sizeof(memdevs) / sizeof(memdevs[0]); i++) {
        if (vfs_register_dev(memdevs[i].name, memdevs[i].ops, NULL) < 0)
            continue;
        int slot = subsys_register(memdevs[i].name, memdevs[i].name,
                                   SUBSYS_CLASS_MEM, 1, memdevs[i].minor);
        subsys_set_state(slot, SUBSYS_STATE_LIVE);
    }
    return 1;
}

/* ---- /dev ------------------------------------------------------------- */
int vfs_register_dev(const char *name, const vfs_ops_t *ops, void *priv)
{
    if (g_dev_count >= VFS_MAX_DEV)
        return -E_NOMEM;

    vfs_node_t *n = &g_dev[g_dev_count++];
    strncpy(n->name, name, VFS_NAME_MAX - 1);
    n->name[VFS_NAME_MAX - 1] = 0;
    n->kind = VFS_CHARDEV;
    n->size = 0;
    n->ops  = ops;
    n->priv = priv;

    dbg_puts("VFS: registered /dev/");
    dbg_puts(n->name);
    dbg_puts("\r\n");
    return 0;
}

static vfs_node_t *dev_lookup(const char *name)
{
    for (unsigned i = 0; i < g_dev_count; i++)
        if (strcmp(g_dev[i].name, name) == 0)
            return &g_dev[i];
    return NULL;
}

/* ---- mount table ------------------------------------------------------
 * A mount is a tmpfs instance attached at an absolute path.  Resolution,
 * readdir and the modifying VFS calls consult this table (longest prefix
 * wins) before falling through to the ext2 root or the /proc overlay, exactly
 * the way the VFS already special-cases /proc.  There is no generic
 * filesystem registry: tmpfs is the only mountable type, which is all a boot
 * needs (OpenRC wants tmpfs on /run, /tmp, /dev/shm, ...).
 */
#define MAX_MOUNTS 8
struct mount_entry {
    char     mnt[GNUOS_PATH_MAX];
    tmpfs_t *fs;
} g_mounts[MAX_MOUNTS];
int g_mount_count = 0;

/* Return the tmpfs instance (if any) that owns `abs`, choosing the longest
 * matching mount prefix, and write the path relative to that instance's root
 * into `rel` (always begins with '/').  Returns NULL when `abs` is on no
 * mount. */
static tmpfs_t *vfs_route_tmpfs(const char *abs, char *rel)
{
    tmpfs_t *best = NULL;
    int      bestlen = -1;
    for (int i = 0; i < g_mount_count; i++) {
        const char *m = g_mounts[i].mnt;
        int ml = (int)strlen(m);
        if (strcmp(abs, m) == 0) {
            if (ml > bestlen) { best = g_mounts[i].fs; bestlen = ml;
                                 rel[0] = '/'; rel[1] = 0; }
        } else if (strncmp(abs, m, ml) == 0 && abs[ml] == '/') {
            if (ml > bestlen) { best = g_mounts[i].fs; bestlen = ml;
                                 strncpy(rel, abs + ml, GNUOS_PATH_MAX - 1);
                                 rel[GNUOS_PATH_MAX - 1] = 0; }
        }
    }
    return best;
}

int vfs_mount_tmpfs(const char *path)
{
    if (g_mount_count >= MAX_MOUNTS)
        return -E_NFILE;
    for (int i = 0; i < g_mount_count; i++)
        if (strcmp(g_mounts[i].mnt, path) == 0)
            return -E_EXIST;
    tmpfs_t *fs = tmpfs_create();
    if (!fs)
        return -E_NOSPC;
    strncpy(g_mounts[g_mount_count].mnt, path, GNUOS_PATH_MAX - 1);
    g_mounts[g_mount_count].mnt[GNUOS_PATH_MAX - 1] = 0;
    g_mounts[g_mount_count].fs = fs;
    g_mount_count++;
    return 0;
}

int vfs_mount_count(void)
{
    return g_mount_count;
}

const char *vfs_mount_path(int i)
{
    if (i < 0 || i >= g_mount_count)
        return 0;
    return g_mounts[i].mnt;
}

int vfs_umount(const char *path)
{
    for (int i = 0; i < g_mount_count; i++) {
        if (strcmp(g_mounts[i].mnt, path) == 0) {
            for (int j = i; j < g_mount_count - 1; j++)
                g_mounts[j] = g_mounts[j + 1];
            g_mount_count--;
            return 0;
        }
    }
    return -E_INVAL;
}

/* ---- path resolution -------------------------------------------------- */
static int resolve(const char *path, vfs_node_t *out, int follow)
{
    if (!path || path[0] != '/')
        return -E_INVAL;

    if (strncmp(path, "/dev/", 5) == 0) {
        vfs_node_t *d = dev_lookup(path + 5);
        if (!d)
            return -E_NOENT;
        *out = *d;
        return 0;
    }

    /* /proc shadows whatever the ext2 image has at that path -- the image
     * carries an empty /proc directory purely so the mount point exists, and
     * the generated tree is what anyone asking for it actually wants. */
    int pr = procfs_resolve(path, out);
    if (pr != -E_INVAL)
        return pr;

    /* A mounted tmpfs shadows the ext2 image at its mount point, exactly as
     * /proc shadows the empty /proc directory the image carries. */
    char mrel[GNUOS_PATH_MAX];
    tmpfs_t *mfs = vfs_route_tmpfs(path, mrel);
    if (mfs)
        return tmpfs_resolve(mfs, mrel, out);

    if (!g_fs_ok)
        return -E_NOENT;

    ext2_dirent_t ent;
    if (!ext2_lookup(&g_fs, path, &ent, follow))
        return -E_NOENT;

    memset(out, 0, sizeof(*out));
    strncpy(out->name, ent.name, VFS_NAME_MAX - 1);
    uint16_t m = (uint16_t)(ent.mode & 0xF000);
    out->kind = (m == EXT2_S_IFDIR) ? VFS_DIR
              : (m == EXT2_S_IFLNK) ? VFS_SYMLINK : VFS_FILE;
    out->size = ent.size;
    out->ops  = (out->kind == VFS_DIR) ? &g_dir_ops : &g_fs_ops;
    out->e2   = ent;
    return 0;
}

/*
 * Fill a Linux-style struct stat.  We do not track timestamps, link counts
 * beyond 1/2, or real device numbers, so those read back as zero/one -- an
 * honest "unknown" beats a fabricated value.  The permission bits come from the
 * inode when present, else from the node kind's default.
 */
static vfs_file_t *get(int h);   /* defined further down; needed by fstat/path */
static void fill_stat(const vfs_node_t *n, lstat_t *st)
{
    memset(st, 0, sizeof(*st));

    uint32_t type, perms;
    switch (n->kind) {
    case VFS_DIR:      type = 0x4000; perms = 0755; break; /* S_IFDIR */
    case VFS_SYMLINK:  type = 0xA000; perms = 0777; break; /* S_IFLNK */
    case VFS_CHARDEV:  type = 0x2000; perms = 0600; break; /* S_IFCHR */
    case VFS_PIPE:     type = 0x1000; perms = 0600; break; /* S_IFIFO */
    case VFS_SOCKET:   type = 0xC000; perms = 0777; break; /* S_IFSOCK */
    default:           type = 0x8000; perms = 0644; break; /* S_IFREG */
    }
    if ((n->e2.mode & 0x0FFF) != 0)
        perms = n->e2.mode & 0x0FFF;

    st->st_dev     = 1;                   /* one mounted volume */
    st->st_ino     = n->e2.ino;
    st->st_nlink   = (n->kind == VFS_DIR) ? 2 : 1;
    st->st_mode    = type | perms;
    st->st_uid     = 0;
    st->st_gid     = 0;
    st->st_rdev    = 0;
    st->st_size    = n->size;
    st->st_blksize = 4096;
    st->st_blocks  = (n->size + 511) / 512;
}

int vfs_stat_linux(const char *path, lstat_t *st, int follow)
{
    vfs_node_t n;
    int r = resolve(path, &n, follow);
    if (r < 0)
        return r;
    fill_stat(&n, st);
    return 0;
}

int vfs_fstat(int h, lstat_t *st)
{
    vfs_file_t *f = get(h);
    if (!f)
        return -E_BADF;
    fill_stat(&f->node, st);
    return 0;
}

/*
 * Pack one musl struct dirent into `p` at `off`, or report that it does not
 * fit.  Returns the record length, or 0 when the caller should stop.  Linux
 * pads every record out to eight bytes and d_off is a cookie pointing just
 * past the entry, which is what a rewinddir-free reader walks by.
 */
static uint32_t emit_dirent(uint8_t *p, uint64_t off, uint32_t len,
                            uint64_t ino, const char *name, uint8_t dt)
{
    uint32_t nl = 0;
    while (nl < 255 && name[nl])
        nl++;
    uint32_t reclen = 19 + nl + 1;      /* d_ino+d_off+d_reclen+d_type+name+NUL */
    reclen = (reclen + 7) & ~7u;

    if (off + reclen > len)
        return 0;

    uint64_t doff = off + reclen;
    uint16_t rl   = (uint16_t)reclen;

    memcpy(p + off + 0,  &ino, 8);
    memcpy(p + off + 8,  &doff, 8);
    memcpy(p + off + 16, &rl, 2);
    memcpy(p + off + 18, &dt, 1);
    for (uint32_t i = 0; i < nl; i++)
        p[off + 19 + i] = (uint8_t)name[i];
    p[off + 19 + nl] = 0;
    return reclen;
}

/* The /proc half of getdents64: no inode, no blocks, just the generated table
 * walked by index.  The inode numbers are synthesised from that index because
 * every reader only needs them to be non-zero and distinct. */
static int64_t proc_getdents64(vfs_file_t *f, void *buf, uint32_t len)
{
    uint8_t *p   = (uint8_t *)buf;
    uint64_t off = 0;

    for (;;) {
        char    name[VFS_NAME_MAX];
        uint8_t dt;
        if (procfs_readdir(f->path, (uint32_t)f->pos, name, &dt) < 0)
            break;

        uint32_t rec = emit_dirent(p, off, len, f->pos + 1, name, dt);
        if (!rec)
            break;                      /* buffer full: pos stays, caller retries */
        off += rec;
        f->pos++;
    }

    return (int64_t)off;
}

/* The tmpfs half of getdents64: enumerate the in-memory tree by index, the
 * same contract proc_getdents64 uses. */
static int64_t tmpfs_getdents64(vfs_file_t *f, void *buf, uint32_t len)
{
    char    rel[GNUOS_PATH_MAX];
    tmpfs_t *fs = vfs_route_tmpfs(f->path, rel);
    if (!fs)
        return -E_NOENT;

    uint8_t *p   = (uint8_t *)buf;
    uint64_t off = 0;

    for (;;) {
        char    name[VFS_NAME_MAX];
        uint8_t dt;
        if (tmpfs_readdir(fs, rel, (uint32_t)f->pos, name, &dt) < 0)
            break;

        uint32_t rec = emit_dirent(p, off, len, f->pos + 1, name, dt);
        if (!rec)
            break;                      /* buffer full: pos stays, caller retries */
        off += rec;
        f->pos++;
    }

    return (int64_t)off;
}

/*
 * getdents64(217).  Emits musl's struct dirent records one variable-length
 * entry at a time and advances the open file's read position by the number of
 * directory entries handed back, so repeated calls walk the whole directory.
 * The toy userland's read()-into-gdirent path is untouched: a directory
 * descriptor is used one way or the other, never both.
 */
int64_t vfs_dir_getdents64(int h, void *buf, uint32_t len)
{
    vfs_file_t *f = get(h);
    if (!f)
        return -E_BADF;
    if (f->node.kind != VFS_DIR)
        return -E_NOTDIR;

    /* A /proc directory has no inode to walk, so it is enumerated by index
     * from the generated table instead of by reading blocks off the disk. */
    if (strncmp(f->path, "/proc", 5) == 0 &&
        (f->path[5] == '\0' || f->path[5] == '/'))
        return proc_getdents64(f, buf, len);

    /* A tmpfs mount is enumerated the same way: by index from its tree. */
    char mrel[GNUOS_PATH_MAX];
    tmpfs_t *mfs = vfs_route_tmpfs(f->path, mrel);
    if (mfs)
        return tmpfs_getdents64(f, buf, len);

    ext2_dir_t     d;
    ext2_dirent_t  e;
    ext2_opendir(&g_fs, f->node.e2.ino, &d);

    uint8_t  *p   = (uint8_t *)buf;
    uint64_t  off = 0;
    uint32_t  total = 0;                /* logical entry index: ".", "..", then real */
    uint32_t  emitted = 0;

    /* getdents64 must report "." and ".." even though ext2_readdir skips
     * them; every reader expects them.  The skip below honours a non-zero
     * f->pos so a rewind-free walk still resumes correctly. */
    const char dot[] = ".", dotdot[] = "..";
    uint32_t parent = ext2_parent_ino(&g_fs, f->node.e2.ino);

    if (total++ >= f->pos) {
        uint32_t rec = emit_dirent(p, off, len, f->node.e2.ino, dot, DT_DIR);
        if (!rec) return (int64_t)off;
        off += rec; emitted++;
    }
    if (total++ >= f->pos) {
        uint32_t rec = emit_dirent(p, off, len, parent, dotdot, DT_DIR);
        if (!rec) return (int64_t)off;
        off += rec; emitted++;
    }

    while (ext2_readdir(&d, &e)) {
        if (total++ < f->pos)
            continue;                   /* reported in a previous call */

        uint32_t rec = emit_dirent(p, off, len, e.ino, e.name,
                                   ext2_is_dir(&e) ? DT_DIR : DT_REG);
        if (!rec)
            break;                      /* buffer full: keep pos, return what we have */

        off += rec;
        emitted++;
    }

    f->pos += emitted;
    return (int64_t)off;
}

int vfs_stat(const char *path, uint64_t *size, int *kind)
{
    vfs_node_t n;
    int r = resolve(path, &n, 1);
    if (r < 0)
        return r;

    if (size)
        *size = n.size;
    if (kind)
        *kind = n.kind;
    return 0;
}

int vfs_unlink(const char *path)
{
    if (strncmp(path, "/dev/", 5) == 0)
        return -E_PERM;
    char rel[GNUOS_PATH_MAX];
    tmpfs_t *fs = vfs_route_tmpfs(path, rel);
    if (fs)
        return tmpfs_unlink(fs, rel);
    if (!g_fs_ok)
        return -E_NOENT;
    return fs_errno(ext2_unlink(&g_fs, path));
}

/*
 * rmdir(84).  The ext2 layer already knows how to remove a directory (see
 * ext2_unlink's isdir branch: it refuses a non-empty one with ENOTEMPTY and
 * drops the parent's ".." link), so this is just the VFS guard that rejects a
 * regular file the way POSIX requires -- unlink(2) is the call for those.
 */
int vfs_rmdir(const char *path)
{
    if (strncmp(path, "/dev/", 5) == 0)
        return -E_PERM;
    char rel[GNUOS_PATH_MAX];
    tmpfs_t *fs = vfs_route_tmpfs(path, rel);
    if (fs)
        return tmpfs_rmdir(fs, rel);

    vfs_node_t n;
    int r = resolve(path, &n, 0);
    if (r < 0)
        return r;
    if (n.kind != GK_DIR)
        return -E_NOTDIR;
    return fs_errno(ext2_unlink(&g_fs, path));
}

int vfs_mkdir(const char *path)
{
    if (strncmp(path, "/dev/", 5) == 0)
        return -E_PERM;
    char rel[GNUOS_PATH_MAX];
    tmpfs_t *fs = vfs_route_tmpfs(path, rel);
    if (fs)
        return tmpfs_mkdir(fs, rel, 0755);
    if (!g_fs_ok)
        return -E_NOENT;
    return fs_errno(ext2_create(&g_fs, path, 1, NULL));
}

int vfs_symlink(const char *target, const char *path)
{
    if (strncmp(path, "/dev/", 5) == 0)
        return -E_PERM;
    char rel[GNUOS_PATH_MAX];
    tmpfs_t *fs = vfs_route_tmpfs(path, rel);
    if (fs)
        return tmpfs_symlink(fs, target, rel);
    return fs_errno(ext2_symlink(&g_fs, target, path));
}

int vfs_readlink(const char *path, char *buf, uint32_t cap)
{
    if (!g_fs_ok)
        return -E_NOENT;
    if (strncmp(path, "/dev/", 5) == 0)
        return -E_INVAL;
    /* ext2_readlink returns the target length (>= 0) on success and a
     * negative EXT2_ code on failure.  Do NOT run the length through
     * fs_errno -- it would map a positive length to -E_INVAL. */
    int r = ext2_readlink(&g_fs, path, buf, cap);
    return r < 0 ? fs_errno(r) : r;
}

int vfs_rename(const char *src, const char *dst)
{
    if (strncmp(src, "/dev/", 5) == 0 || strncmp(dst, "/dev/", 5) == 0)
        return -E_PERM;
    return fs_errno(ext2_rename(&g_fs, src, dst));
}

/* ---- open-file table -------------------------------------------------- */
static vfs_file_t *get(int h)
{
    if (h < 0 || h >= VFS_MAX_FILES || g_files[h].refs <= 0)
        return NULL;
    return &g_files[h];
}

static int slot_alloc(void)
{
    for (int h = 0; h < VFS_MAX_FILES; h++)
        if (!g_files[h].refs)
            return h;
    return -E_MFILE;
}

int vfs_file_open(const char *path, int flags)
{
    vfs_node_t node;
    int r = resolve(path, &node, 1);

    if (r == -E_NOENT && (flags & O_CREAT)) {
        char rel[GNUOS_PATH_MAX];
        tmpfs_t *fs = vfs_route_tmpfs(path, rel);
        if (fs) {
            int c = tmpfs_create_file(fs, rel, 0644);
            if (c < 0)
                return c;
            r = resolve(path, &node, 1);
        } else if (g_fs_ok) {
            int c = ext2_create(&g_fs, path, 0, NULL);
            if (c != EXT2_OK)
                return fs_errno(c);
            r = resolve(path, &node, 1);
        }
    }
    if (r < 0)
        return r;

    if ((flags & O_TRUNC) && node.kind == VFS_FILE && node.size) {
        char trel[GNUOS_PATH_MAX];
        tmpfs_t *tfs = vfs_route_tmpfs(path, trel);
        if (tfs)
            tmpfs_truncate(tfs, trel);
        else
            ext2_truncate(&g_fs, &node.e2);
        node.size = 0;
    }

    int h = slot_alloc();
    if (h < 0)
        return h;

    g_files[h].refs  = 1;
    g_files[h].node  = node;
    g_files[h].pos   = (flags & O_APPEND) ? node.size : 0;
    g_files[h].flags = flags;
    strncpy(g_files[h].path, path, VFS_PATH_MAX - 1);
    g_files[h].path[VFS_PATH_MAX - 1] = 0;
    return h;
}

const char *vfs_file_path(int h)
{
    vfs_file_t *f = get(h);
    return f ? f->path : NULL;
}

const vfs_ops_t *vfs_file_ops(int h)
{
    vfs_file_t *f = get(h);
    return f ? f->node.ops : NULL;
}

const vfs_node_t *vfs_file_node(int h)
{
    vfs_file_t *f = get(h);
    return f ? &f->node : NULL;
}

int vfs_file_flags(int h)
{
    vfs_file_t *f = get(h);
    return f ? f->flags : 0;
}

void vfs_file_setfl(int h, int nonblock)
{
    vfs_file_t *f = get(h);
    if (!f)
        return;
    f->flags = (f->flags & ~O_NONBLOCK) | (nonblock ? O_NONBLOCK : 0);
}

int vfs_chmod(const char *path, uint32_t mode)
{
    char rel[GNUOS_PATH_MAX];
    tmpfs_t *fs = vfs_route_tmpfs(path, rel);
    if (fs)
        return tmpfs_chmod(fs, rel, mode);

    vfs_node_t n;
    int r = resolve(path, &n, 1);
    if (r < 0)
        return r;

    /* Device nodes and pipes have no inode to write the bits back to; report
     * success anyway, because the caller only ever loosens permissions we do
     * not enforce in the first place. */
    if (!n.e2.ino)
        return 0;

    return fs_errno(ext2_chmod(&g_fs, &n.e2, (uint16_t)mode));
}

int vfs_pipe(int *read_handle, int *write_handle)
{
    pipe_t *p = NULL;
    for (int i = 0; i < VFS_MAX_PIPES; i++) {
        if (!g_pipes[i].used) {
            p = &g_pipes[i];
            break;
        }
    }
    if (!p)
        return -E_NOMEM;

    /* Both ends must be reservable, or neither is. */
    int rh = slot_alloc();
    if (rh < 0)
        return rh;
    g_files[rh].refs = 1;                    /* claim it while we look again */

    int wh = slot_alloc();
    if (wh < 0) {
        g_files[rh].refs = 0;
        return wh;
    }

    p->used    = 1;
    p->readers = 1;
    p->writers = 1;
    p->head = p->tail = p->count = 0;

    for (int k = 0; k < 2; k++) {
        int h = k ? wh : rh;
        memset(&g_files[h], 0, sizeof(g_files[h]));
        g_files[h].refs  = 1;
        g_files[h].flags = k ? O_WRONLY : O_RDONLY;
        strncpy(g_files[h].node.name, "pipe", VFS_NAME_MAX - 1);
        g_files[h].node.kind = VFS_PIPE;
        g_files[h].node.ops  = &g_pipe_ops;
        g_files[h].node.priv = p;
    }

    *read_handle  = rh;
    *write_handle = wh;
    return 0;
}

/* read()/write() on a socket are recvfrom()/sendto() with no address; the
 * socket index rides in node.priv, which is why sock.c can implement these
 * two without ever seeing the open-file table. */
static const vfs_ops_t g_sock_ops = { .read = sock_node_read, .write = sock_node_write };

int vfs_socket(int sock_index)
{
    int h = slot_alloc();
    if (h < 0)
        return h;

    memset(&g_files[h], 0, sizeof(g_files[h]));
    g_files[h].refs  = 1;
    g_files[h].flags = O_RDWR;
    strncpy(g_files[h].node.name, "socket", VFS_NAME_MAX - 1);
    g_files[h].node.kind = VFS_SOCKET;
    g_files[h].node.ops  = &g_sock_ops;
    g_files[h].node.priv = (void *)(uintptr_t)sock_index;
    return h;
}

int vfs_file_sock(int h)
{
    vfs_file_t *f = get(h);
    if (!f || f->node.kind != VFS_SOCKET)
        return -1;
    return (int)(uintptr_t)f->node.priv;
}

void vfs_file_ref(int h)
{
    vfs_file_t *f = get(h);
    if (!f)
        return;

    f->refs++;
    if (f->node.kind == VFS_PIPE) {
        pipe_t *p = (pipe_t *)f->node.priv;
        if (f->flags == O_WRONLY)
            p->writers++;
        else
            p->readers++;
    }
}

void vfs_file_unref(int h)
{
    vfs_file_t *f = get(h);
    if (!f)
        return;

    /* The last descriptor onto a socket closes it -- and for TCP that is not
     * a bookkeeping detail, it is what sends the FIN. */
    if (f->node.kind == VFS_SOCKET) {
        if (--f->refs <= 0) {
            f->refs = 0;
            sock_close((int)(uintptr_t)f->node.priv);
        }
        return;
    }

    if (f->node.kind == VFS_PIPE) {
        pipe_t *p = (pipe_t *)f->node.priv;

        if (f->flags == O_WRONLY) {
            if (p->writers > 0)
                p->writers--;
        } else if (p->readers > 0) {
            p->readers--;
        }

        /* Somebody may be asleep waiting for exactly this. */
        sched_wake_reason(WAIT_PIPE);

        if (--f->refs <= 0) {
            f->refs = 0;
            if (p->readers == 0 && p->writers == 0)
                p->used = 0;
        }
        return;
    }

    if (--f->refs <= 0)
        f->refs = 0;
}

int32_t vfs_file_read(int h, void *buf, uint32_t len)
{
    vfs_file_t *f = get(h);
    if (!f)
        return -E_BADF;
    if ((f->flags & 3) == O_WRONLY)
        return -E_BADF;
    if (!f->node.ops || !f->node.ops->read)
        return -E_INVAL;

    /* Only files and directories have a position at all; a device, a pipe
     * and a socket are streams, and advancing an offset on them would be
     * inventing a number nobody can use. */
    int32_t n = f->node.ops->read(&f->node, f->pos, buf, len);
    if (n > 0 && (f->node.kind == VFS_FILE || f->node.kind == VFS_DIR))
        f->pos += (uint32_t)n;
    return n;
}

int32_t vfs_file_write(int h, const void *buf, uint32_t len)
{
    vfs_file_t *f = get(h);
    if (!f)
        return -E_BADF;
    if ((f->flags & 3) == O_RDONLY)
        return -E_BADF;
    if (!f->node.ops || !f->node.ops->write)
        return -E_INVAL;

    int32_t n = f->node.ops->write(&f->node, f->pos, buf, len);
    if (n > 0 && (f->node.kind == VFS_FILE || f->node.kind == VFS_DIR)) {
        f->pos += (uint32_t)n;
        if (f->pos > f->node.size)
            f->node.size = f->pos;
    }
    return n;
}

/* pread/pwrite differ from read/write in exactly one way -- the position is
 * an argument instead of state -- so they share the permission and ops checks
 * and then decline to touch f->pos.  A pipe or socket is refused rather than
 * silently served from offset 0: a caller that passes an offset is asking for
 * random access, and answering a stream read instead would be a wrong answer
 * dressed up as a right one.  A character device is *not* refused: /dev/fb0
 * and /dev/mem are addressable, and pread is the natural way to reach into
 * them. */
static int32_t pio_check(vfs_file_t *f)
{
    if (!f)
        return -E_BADF;
    if (f->node.kind == VFS_PIPE || f->node.kind == VFS_SOCKET)
        return -E_SPIPE;
    return 0;
}

int32_t vfs_file_pread(int h, void *buf, uint32_t len, uint64_t off)
{
    vfs_file_t *f = get(h);
    int32_t     e = pio_check(f);
    if (e != 0)
        return e;
    if ((f->flags & 3) == O_WRONLY)
        return -E_BADF;
    if (!f->node.ops || !f->node.ops->read)
        return -E_INVAL;
    return f->node.ops->read(&f->node, off, buf, len);
}

int32_t vfs_file_pwrite(int h, const void *buf, uint32_t len, uint64_t off)
{
    vfs_file_t *f = get(h);
    int32_t     e = pio_check(f);
    if (e != 0)
        return e;
    if ((f->flags & 3) == O_RDONLY)
        return -E_BADF;
    if (!f->node.ops || !f->node.ops->write)
        return -E_INVAL;

    int32_t n = f->node.ops->write(&f->node, off, buf, len);
    /* The offset does not move, but the file can still have grown, and a
     * later fstat() has to see that. */
    if (n > 0 && f->node.kind == VFS_FILE && off + (uint32_t)n > f->node.size)
        f->node.size = off + (uint32_t)n;
    return n;
}

int64_t vfs_file_seek(int h, int64_t off, int whence)
{
    vfs_file_t *f = get(h);
    if (!f)
        return -E_BADF;
    if (f->node.kind == VFS_PIPE || f->node.kind == VFS_CHARDEV ||
        f->node.kind == VFS_SOCKET)
        return -E_INVAL;

    int64_t base;
    switch (whence) {
    case 0: base = 0;                     break;   /* SEEK_SET */
    case 1: base = (int64_t)f->pos;       break;   /* SEEK_CUR */
    case 2: base = (int64_t)f->node.size; break;   /* SEEK_END */
    default: return -E_INVAL;
    }
    if (base + off < 0)
        return -E_INVAL;
    f->pos = (uint64_t)(base + off);
    return (int64_t)f->pos;
}

/* The node kind backing a handle -- used by poll(7)/ppoll(271) to decide
 * whether a descriptor is readable without having to open it. */
uint8_t vfs_file_kind(int h)
{
    vfs_file_t *f = get(h);
    return f ? f->node.kind : 0;
}

/* True when a read on a pipe handle would not block: either bytes are
 * buffered, or the write end has been closed (EOF). */
int vfs_pipe_readable(int h)
{
    vfs_file_t *f = get(h);
    if (!f || f->node.kind != VFS_PIPE)
        return 0;
    pipe_t *p = (pipe_t *)f->node.priv;
    if (p->count > 0)
        return 1;
    if (p->writers == 0)
        return 1;                       /* read end of a closed pipe => EOF */
    return 0;
}

int vfs_read_all(const char *path, void *buf, uint32_t cap, uint32_t *out_size)
{
    int h = vfs_file_open(path, O_RDONLY);
    if (h < 0)
        return 0;

    uint32_t total = 0;
    while (total < cap) {
        int32_t n = vfs_file_read(h, (uint8_t *)buf + total, cap - total);
        if (n <= 0)
            break;
        total += (uint32_t)n;
    }

    /* Filling the buffer exactly is ambiguous: the file may have ended there,
     * or there may be more. Probe one further byte to tell the two apart.
     * Silently returning a short read would hand the caller a truncated image
     * that then fails much later as a malformed ELF. */
    int truncated = 0;
    if (total == cap) {
        uint8_t probe;
        truncated = vfs_file_read(h, &probe, 1) > 0;
    }
    vfs_file_unref(h);

    if (truncated) {
        dbg_puts("VFS: ");
        dbg_puts(path);
        dbg_puts(" is larger than the ");
        dbg_puts_dec(cap);
        dbg_puts(" byte read buffer\n");
        return 0;
    }

    if (out_size)
        *out_size = total;
    return total > 0;
}
