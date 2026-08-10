/*
 * fat.h — read/write FAT12/FAT16/FAT32 driver over an in-memory image. (GPLv2)
 *
 * The image is the initrd the bootloader dropped in RAM, so there is no block
 * layer underneath: every "sector read" is just a pointer into the image, and
 * every write is a store to that same RAM.  The volume is therefore genuinely
 * writable -- mkdir, rm and friends really do modify the FAT and the directory
 * entries -- but only until the machine is switched off.
 *
 * Long file names are not generated or parsed; everything uses the classic
 * 8.3 short names.
 */
#ifndef GNUCOS_FAT_H
#define GNUCOS_FAT_H

#include <stdint.h>

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F

/* Failure codes from the mutating calls; the VFS maps them onto errno. */
#define FAT_OK          0
#define FAT_ENOENT     (-1)
#define FAT_EEXIST     (-2)
#define FAT_ENOSPC     (-3)
#define FAT_ENOTDIR    (-4)
#define FAT_ENOTEMPTY  (-5)
#define FAT_EINVAL     (-6)

typedef struct {
    uint8_t *img;
    uint32_t img_size;

    uint16_t bytes_per_sec;
    uint8_t  sec_per_clus;
    uint8_t  num_fats;
    uint16_t reserved_secs;
    uint16_t root_entries;      /* 0 on FAT32 */
    uint32_t sec_per_fat;
    uint32_t total_secs;

    uint32_t fat_start;         /* first FAT sector                       */
    uint32_t root_start;        /* FAT12/16: first root-directory sector  */
    uint32_t root_secs;         /* FAT12/16: size of the root dir region  */
    uint32_t root_clus;         /* FAT32:    first cluster of the root dir */
    uint32_t data_start;        /* first sector of the data region        */
    uint32_t clus_count;

    uint32_t alloc_hint;        /* where the last allocation stopped */
    int      type;              /* 12, 16 or 32 */
} fat_fs_t;

typedef struct {
    char     name[13];          /* "INIT.ELF", NUL-terminated */
    uint8_t  attr;
    uint32_t first_clus;
    uint32_t size;
    /* Byte offset of the raw 32-byte entry inside the image.  Zero means
     * "synthesised" (the root directory), which cannot be modified. */
    uint32_t ent_off;
} fat_dirent_t;

/* Directory cursor; works for both the fixed FAT12/16 root region and a
 * regular cluster-chained directory. */
typedef struct {
    fat_fs_t *fs;
    int      fixed_root;
    uint32_t clus;              /* current cluster (chained directories) */
    uint32_t index;             /* 32-byte entry index within the region */
} fat_dir_t;

/* Parse the BPB.  Returns 1 on success, 0 if the image is not a FAT volume. */
int fat_mount(fat_fs_t *fs, uint8_t *img, uint32_t img_size);

/* Start iterating a directory.  clus == 0 means "the root directory". */
void fat_opendir(fat_fs_t *fs, uint32_t clus, fat_dir_t *dir);

/* Fetch the next usable entry.  Returns 1 on success, 0 at end of directory. */
int fat_readdir(fat_dir_t *dir, fat_dirent_t *out);

/* Resolve an absolute path such as "/dev/init.elf" (case-insensitive, 8.3). */
int fat_lookup(fat_fs_t *fs, const char *path, fat_dirent_t *out);

/* Copy up to `len` bytes starting at `off` from the file described by `ent`.
 * Returns the number of bytes actually copied. */
uint32_t fat_read(fat_fs_t *fs, const fat_dirent_t *ent,
                  uint32_t off, void *buf, uint32_t len);

/* ---- the mutating half ------------------------------------------------ */

/* Store `len` bytes at `off`, growing the cluster chain and the recorded file
 * size as needed.  `ent` is updated in place and written back to the on-disk
 * directory entry.  Returns the number of bytes stored. */
uint32_t fat_write(fat_fs_t *fs, fat_dirent_t *ent,
                   uint32_t off, const void *buf, uint32_t len);

/* Create an empty file (attr == 0 or FAT_ATTR_ARCHIVE) or a directory
 * (FAT_ATTR_DIRECTORY, which also gets its "." and ".." entries). */
int fat_create(fat_fs_t *fs, const char *path, uint8_t attr, fat_dirent_t *out);

/* Remove a file, or an empty directory. */
int fat_unlink(fat_fs_t *fs, const char *path);

/* Drop every cluster of a file and reset its size to zero. */
int fat_truncate(fat_fs_t *fs, fat_dirent_t *ent);

#endif
