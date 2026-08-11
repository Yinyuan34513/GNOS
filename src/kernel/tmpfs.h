/*
 * tmpfs.h — an in-memory filesystem for the mount(2) syscall. (GPLv2)
 *
 * This is the only filesystem type our mount() supports: OpenRC and friends
 * want to put tmpfs on /run, /tmp, /dev/shm and the like, and that is exactly
 * what a RAM-backed tree of directories and files provides.  It is deliberately
 * small: no hard links, no real permissions enforcement, no mmap — just enough
 * that a boot script can mkdir /run/lock, write a state file, and read it back.
 *
 * Storage is a fixed static node pool plus a fixed static data arena, so the
 * filesystem needs no kernel allocator and can never fragment.  Unlinked file
 * data is leaked (not reclaimed) — acceptable for a toy kernel where tmpfs
 * lives only for the lifetime of one boot.
 */
#ifndef GNUCOS_TMPFS_H
#define GNUCOS_TMPFS_H

#include <stdint.h>
#include "vfs.h"

/* Opaque tmpfs instance: an empty root directory plus a running inode counter. */
typedef struct tmpfs_inst tmpfs_t;

/* Create a fresh, empty tmpfs instance.  Returns NULL when the static node
 * pool is exhausted. */
tmpfs_t *tmpfs_create(void);

/* Resolve `rel` (a path starting with '/', relative to the instance root) to
 * a vfs_node.  Returns 0 on success, -E_NOENT when missing, -E_INVAL when
 * `rel` is not absolute. */
int tmpfs_resolve(tmpfs_t *fs, const char *rel, vfs_node_t *out);

/* Enumerate a tmpfs directory for getdents64.  `index` is 0-based; index 0 is
 * "." and 1 is "..", then the real children.  Fills name/type and returns 0,
 * or -E_NOENT once the directory is exhausted. */
int tmpfs_readdir(tmpfs_t *fs, const char *rel, uint32_t index,
                  char *name, uint8_t *type);

/* Mutating operations; return 0 or a negative errno. */
int tmpfs_mkdir(tmpfs_t *fs, const char *rel, uint32_t mode);
int tmpfs_create_file(tmpfs_t *fs, const char *rel, uint32_t mode);
int tmpfs_unlink(tmpfs_t *fs, const char *rel);
int tmpfs_rmdir(tmpfs_t *fs, const char *rel);
int tmpfs_symlink(tmpfs_t *fs, const char *target, const char *rel);
int tmpfs_chmod(tmpfs_t *fs, const char *rel, uint32_t mode);
/* Rename within one tmpfs instance; the caller has already established that
 * both paths live on the same mount. */
int tmpfs_rename(tmpfs_t *fs, const char *srel, const char *drel);
/* Truncate the file at `rel` to zero length (payload bytes are leaked). */
int tmpfs_truncate(tmpfs_t *fs, const char *rel);
/* truncate(2) to an arbitrary length, zero-filling in both directions. */
int tmpfs_setsize(tmpfs_t *fs, const char *rel, uint64_t len);
/* readlink(2): returns the target length, or a negative errno. */
int tmpfs_readlink(tmpfs_t *fs, const char *rel, char *buf, uint32_t cap);

#endif
