/*
 * ext2.h — read/write ext2 driver over an in-memory image. (GPLv2)
 *
 * The image is the initrd the bootloader dropped in RAM, so there is no block
 * layer underneath: a "block read" is just a pointer into the image, and a
 * write is a store to that same RAM.  The volume is genuinely writable --
 * mkdir, rm and friends really do allocate blocks and inodes and rewrite the
 * bitmaps -- but only until the machine is switched off.
 *
 * ext3 images mount too.  ext3 is ext2 plus a journal, and the journal lives
 * in an ordinary inode described by a *compatible* feature flag, which by
 * definition an implementation may ignore.  We ignore it: nothing here needs
 * crash recovery when the filesystem evaporates at power-off anyway.
 *
 * What we deliberately do not implement: symlinks, extended attributes, and
 * HTree directory indexes (the image is built with ^dir_index so no directory
 * is ever indexed).  Anything with an unknown *incompatible* feature bit --
 * ext4 extents above all -- is refused at mount time rather than silently
 * misread.
 */
#ifndef GNUCOS_EXT2_H
#define GNUCOS_EXT2_H

#include <stdint.h>

#define EXT2_SUPER_OFF     1024      /* the superblock never moves */
#define EXT2_MAGIC         0xEF53
#define EXT2_ROOT_INO      2
#define EXT2_GOOD_OLD_ISIZE 128      /* rev 0 has no s_inode_size field */
#define EXT2_NAME_MAX      64        /* what we report, not what we match */

/* i_mode type bits */
#define EXT2_S_IFMT        0xF000
#define EXT2_S_IFREG       0x8000
#define EXT2_S_IFDIR       0x4000
#define EXT2_S_IFLNK       0xA000

/* directory entry file_type values (the `filetype` feature) */
#define EXT2_FT_REG        1
#define EXT2_FT_DIR        2
#define EXT2_FT_SYMLINK    7

/* i_block[] layout: 12 direct, then one of each indirect level */
#define EXT2_NDIR_BLOCKS   12
#define EXT2_IND_BLOCK     12
#define EXT2_DIND_BLOCK    13
#define EXT2_TIND_BLOCK    14
#define EXT2_N_BLOCKS      15

/*
 * The only incompatible feature we can cope with.  FILETYPE merely says the
 * one-byte file_type field in a directory entry is meaningful; we read it
 * when present and fall back to the inode's mode when it is not.
 */
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002

/* Failure codes from the mutating calls; the VFS maps them onto errno. */
#define EXT2_OK          0
#define EXT2_ENOENT     (-1)
#define EXT2_EEXIST     (-2)
#define EXT2_ENOSPC     (-3)
#define EXT2_ENOTDIR    (-4)
#define EXT2_ENOTEMPTY  (-5)
#define EXT2_EINVAL     (-6)
#define EXT2_EISDIR     (-7)

typedef struct {
    uint8_t *img;
    uint32_t img_size;

    uint32_t block_size;
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t first_data_block;   /* 1 when block_size == 1024, else 0 */
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t first_ino;          /* first inode a file may use (11 on rev 1) */
    uint32_t group_count;
    uint32_t gdt_off;            /* byte offset of the group descriptor table */

    uint32_t alloc_hint;         /* block number the last search stopped at */
    int      has_filetype;       /* directory entries carry a type byte */
} ext2_fs_t;

/*
 * A resolved name.  `ino` is the handle everything else takes; the rest is a
 * snapshot of the inode taken at lookup time, which ext2_write() keeps in
 * step as the file grows.
 */
typedef struct {
    char     name[EXT2_NAME_MAX];  /* leaf name, NUL-terminated */
    uint32_t ino;                  /* 0 means "no such entry" */
    uint32_t size;
    uint16_t mode;                 /* i_mode, so EXT2_S_IFDIR & co. apply */
} ext2_dirent_t;

/* Directory cursor: a byte offset into the directory's own data stream. */
typedef struct {
    ext2_fs_t *fs;
    uint32_t   ino;
    uint32_t   off;
    uint32_t   size;
} ext2_dir_t;

/* Parse the superblock.  Returns 1 on success, 0 if this is not an ext2/3
 * volume or it uses features we would misread. */
int ext2_mount(ext2_fs_t *fs, uint8_t *img, uint32_t img_size);

/* Start iterating a directory.  ino == 0 means the root directory. */
void ext2_opendir(ext2_fs_t *fs, uint32_t ino, ext2_dir_t *dir);

/* Fetch the next entry, skipping "." and "..".  1 on success, 0 at the end. */
int ext2_readdir(ext2_dir_t *dir, ext2_dirent_t *out);

/* The inode a directory's ".." entry names (root's parent is itself).  Used
 * to emit ".." from getdents64, which must report it even though ext2_readdir
 * skips it. */
uint32_t ext2_parent_ino(ext2_fs_t *fs, uint32_t ino);

/* Resolve an absolute path such as "/init.elf".  Case sensitive, as ext2 is.
 * When `follow_final` is set, a symlink at the very end of the path is
 * followed; when clear, the symlink inode itself is returned (lstat/readlink). */
int ext2_lookup(ext2_fs_t *fs, const char *path, ext2_dirent_t *out,
                int follow_final);

/* True when the entry names a directory. */
int ext2_is_dir(const ext2_dirent_t *ent);

/* Copy up to `len` bytes from `off`.  Returns the number of bytes copied. */
uint32_t ext2_read(ext2_fs_t *fs, const ext2_dirent_t *ent,
                   uint32_t off, void *buf, uint32_t len);

/* ---- the mutating half ------------------------------------------------ */

/* Store `len` bytes at `off`, allocating blocks and growing i_size as needed.
 * `ent->size` is updated to match the inode.  Returns bytes stored. */
uint32_t ext2_write(ext2_fs_t *fs, ext2_dirent_t *ent,
                    uint32_t off, const void *buf, uint32_t len);

/* Create an empty regular file, or a directory complete with "." and "..". */
int ext2_create(ext2_fs_t *fs, const char *path, int isdir, ext2_dirent_t *out);

/* Create a symbolic link `path` pointing at `target`. */
int ext2_symlink(ext2_fs_t *fs, const char *target, const char *path);

/* Read a symlink's target into `buf` (size `cap`).  Returns the length, or a
 * negative EXT2_* code on error. */
int ext2_readlink(ext2_fs_t *fs, const char *path, char *buf, uint32_t cap);

/* Rename `src` to `dst`.  Handles file/file, file/overwrite, dir/dir
 * (refusing a non-empty destination), and dir/empty-dir. */
int ext2_rename(ext2_fs_t *fs, const char *src, const char *dst);

/* Remove a file, or an empty directory. */
int ext2_unlink(ext2_fs_t *fs, const char *path);

/* Release every block of a file and reset its size to zero. */
int ext2_truncate(ext2_fs_t *fs, ext2_dirent_t *ent);

/* Replace the permission bits (low 12) of an inode; the type bits survive. */
int ext2_chmod(ext2_fs_t *fs, ext2_dirent_t *ent, uint16_t mode);

#endif
