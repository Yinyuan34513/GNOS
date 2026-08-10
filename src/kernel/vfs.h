/*
 * vfs.h — virtual file system. (GPLv2)
 *
 * A deliberately small VFS: one ext2 volume mounted on "/", a flat set of
 * character devices under "/dev", and anonymous pipes that exist only in the
 * open-file table.  That is exactly what the POSIX syscall layer needs in
 * order to give user space a uniform open/read/write/close view of the
 * initrd, the tty and a pipeline alike, without the syscalls having to know
 * which of the three a descriptor happens to name.
 */
#ifndef GNUCOS_VFS_H
#define GNUCOS_VFS_H

#include <stdint.h>

#include "ext2.h"
#include "sysnum.h"          /* O_* flags and the user-visible record types */

/* The open-file table is shared by every process, so it has to cover the
 * worst case across all of them at once, not per process. */
#define VFS_MAX_FILES 64
#define VFS_MAX_DEV   8
#define VFS_MAX_PIPES 16
#define VFS_NAME_MAX  32

/* Pipe capacity.  POSIX only promises 512 bytes of atomicity; a page is
 * plenty to keep a producer running while its consumer is descheduled. */
#define VFS_PIPE_CAP  4096

/* node kinds -- the same numbers stat() reports as GK_* */
#define VFS_FILE      1
#define VFS_DIR       2
#define VFS_CHARDEV   3
#define VFS_PIPE      4
#define VFS_SOCKET    5

/* errno values the syscall layer hands back to user space */
#define E_PERM        1
#define E_NOENT       2
#define E_SRCH        3
#define E_INTR        4
#define E_IO          5
#define E_BADF        9
#define E_CHILD      10
#define E_AGAIN      11
#define E_NOMEM      12
#define E_FAULT      14
#define E_EXIST      17
#define E_XDEV       18
#define E_NOTDIR     20
#define E_ISDIR      21
#define E_INVAL      22
#define E_MFILE      24
#define E_NOTTY      25
#define E_NODEV      19
#define E_NOSPC      28
#define E_PIPE       32
#define E_RANGE      34
#define E_NAMETOOLONG 36
#define E_NOSYS      38
#define E_NOTEMPTY   39

/* Sockets bring their own half of the errno table with them.  These are the
 * Linux numbers, not invented ones: musl maps a negative return straight into
 * errno, so getting ECONNREFUSED wrong here makes connect() report something
 * unrelated -- and BusyBox branches on the difference. */
#define E_NOTSOCK        88
#define E_DESTADDRREQ    89
#define E_MSGSIZE        90
#define E_PROTOTYPE      91
#define E_NOPROTOOPT     92
#define E_PROTONOSUPPORT 93
#define E_SOCKTNOSUPPORT 94
#define E_OPNOTSUPP      95
#define E_AFNOSUPPORT    97
#define E_ADDRINUSE      98
#define E_ADDRNOTAVAIL   99
#define E_NETUNREACH    101
#define E_CONNABORTED   103
#define E_CONNRESET     104
#define E_NOBUFS        105
#define E_ISCONN        106
#define E_NOTCONN       107
#define E_TIMEDOUT      110
#define E_CONNREFUSED   111
#define E_HOSTUNREACH   113
#define E_ALREADY       114
#define E_INPROGRESS    115

struct vfs_node;

typedef struct {
    int32_t (*read)(struct vfs_node *n, uint64_t off, void *buf, uint32_t len);
    int32_t (*write)(struct vfs_node *n, uint64_t off, const void *buf, uint32_t len);
} vfs_ops_t;

typedef struct vfs_node {
    char             name[VFS_NAME_MAX];
    int              kind;
    uint64_t         size;
    const vfs_ops_t *ops;
    void            *priv;
    ext2_dirent_t    e2;         /* valid when the node lives on the ext2 mount */
} vfs_node_t;

/* The absolute path a descriptor was opened with.  Lets fchdir() and a
 * relative openat() rebuild a path from a directory fd without re-walking
 * from the root every time. */
#define VFS_PATH_MAX  GNUOS_PATH_MAX

/* Mount the ext2 image at `img` on "/".  Returns 1 on success. */
int  vfs_init(uint8_t *img, uint32_t img_size);

/* Publish a character device as "/dev/<name>". */
int  vfs_register_dev(const char *name, const vfs_ops_t *ops, void *priv);

/* ---- names ------------------------------------------------------------ */
int  vfs_stat(const char *path, uint64_t *size, int *kind);
int  vfs_unlink(const char *path);
int  vfs_mkdir(const char *path);

/* Linux-style stat (144-byte struct) used by musl/BusyBox. */
int  vfs_stat_linux(const char *path, lstat_t *st);
int  vfs_fstat(int h, lstat_t *st);

/* The absolute path a descriptor names (for fchdir / relative openat).
 * Returns NULL if the handle is not open.  The pointer is into the open-file
 * table and only valid until the next VFS call. */
const char *vfs_file_path(int h);

/* The device operations behind a descriptor, or NULL for a plain file, a
 * directory, or a closed handle.  ioctl() uses this to tell "is this fd the
 * terminal?" without hard-coding a path. */
const vfs_ops_t *vfs_file_ops(int h);

/* Change the permission bits (low 12) of a path's inode; returns 0 or errno. */
int  vfs_chmod(const char *path, uint32_t mode);

/* getdents64(217): fill a musl struct dirent buffer from a directory fd. */
int64_t vfs_dir_getdents64(int h, void *buf, uint32_t len);

/*
 * Open-file table.  A "handle" is an index into a global table of open file
 * descriptions, each with a reference count -- exactly the POSIX model, and
 * the reason fork() can hand a child the same file offset as its parent by
 * doing nothing more than bumping a counter.  Per-process fd numbers live in
 * the PCB and map onto these handles.
 */
int     vfs_file_open(const char *path, int flags);
int32_t vfs_file_read(int h, void *buf, uint32_t len);
int32_t vfs_file_write(int h, const void *buf, uint32_t len);
int64_t vfs_file_seek(int h, int64_t off, int whence);
void    vfs_file_ref(int h);
void    vfs_file_unref(int h);
uint8_t vfs_file_kind(int h);
int     vfs_pipe_readable(int h);

/*
 * Create a pipe: two handles onto one kernel buffer.  The buffer stays alive
 * exactly as long as somebody holds one of the ends, which is what turns
 * "the writer closed" into an end-of-file for the reader.
 */
int vfs_pipe(int *read_handle, int *write_handle);

/*
 * Wrap a socket -- an index into sock.c's table -- in an open-file handle.
 *
 * Sockets are not names and never appear in a directory, but everything a
 * descriptor can do to them (read, write, dup, close, poll) is already
 * implemented once in the open-file table, and a socket that lived outside it
 * would need every one of those written a second time.  The handle *owns* the
 * socket from here on: the last unref closes it, which is what makes close(2)
 * on the last dup tear down a TCP connection.
 */
int vfs_socket(int sock_index);

/* The socket index behind a handle, or -1 if the handle is not a socket.
 * This is how the syscall layer tells socket calls from file calls without
 * keeping a second table of which fd is which. */
int vfs_file_sock(int h);

/* The access-mode / status flags (O_* bits) of a handle.  fcntl(F_GETFL)
 * needs this; for sockets it carries O_NONBLOCK in step with sock_set_nonblock
 * so that F_GETFL/F_SETFL round-trip the non-blocking bit exactly. */
int vfs_file_flags(int h);
/* Set the O_NONBLOCK bit of a handle's flags, preserving the access mode. */
void vfs_file_setfl(int h, int nonblock);

/* Convenience wrapper used by the boot path: slurp a whole file. */
int vfs_read_all(const char *path, void *buf, uint32_t cap, uint32_t *out_size);

#endif
