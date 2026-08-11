/*
 * ext2.c — an ext2/ext3 driver that works directly on a RAM image. (GPLv2)
 *
 * Layout recap, because every routine below is an expression of it:
 *
 *   byte 1024        superblock
 *   block  N+1       group descriptor table (N = first data block)
 *   per group        block bitmap, inode bitmap, inode table, then data
 *
 * A file's data blocks are found through i_block[]: twelve direct entries and
 * then one single-, one double- and one triple-indirect block.  That is the
 * whole of the "allocation" story; there are no extents and no b-trees.
 *
 * Since the image is already in memory, every lookup ends in a pointer rather
 * than an I/O, so the code reads as pointer arithmetic over a byte array.  All
 * multi-byte fields are little-endian and are touched only through the rd/wr
 * helpers, which keeps us honest about alignment: directory entries are only
 * 4-byte aligned, and their names make the following entry start anywhere.
 */
#include <stddef.h>
#include <stdint.h>

#include "ext2.h"
#include "kstring.h"

/* ---- superblock field offsets ----------------------------------------- */
#define SB_INODES_COUNT      0
#define SB_BLOCKS_COUNT      4
#define SB_FREE_BLOCKS      12
#define SB_FREE_INODES      16
#define SB_FIRST_DATA_BLOCK 20
#define SB_LOG_BLOCK_SIZE   24
#define SB_BLOCKS_PER_GROUP 32
#define SB_INODES_PER_GROUP 40
#define SB_WTIME            48
#define SB_MAGIC            56
#define SB_REV_LEVEL        76
#define SB_FIRST_INO        84
#define SB_INODE_SIZE       88
#define SB_FEATURE_INCOMPAT 96

/* ---- group descriptor field offsets ----------------------------------- */
#define GD_SIZE             32
#define GD_BLOCK_BITMAP      0
#define GD_INODE_BITMAP      4
#define GD_INODE_TABLE       8
#define GD_FREE_BLOCKS      12
#define GD_FREE_INODES      14
#define GD_USED_DIRS        16

/* ---- inode field offsets ---------------------------------------------- */
#define I_MODE               0
#define I_SIZE               4
#define I_ATIME              8
#define I_CTIME             12
#define I_MTIME             16
#define I_DTIME             20
#define I_LINKS             26
#define I_BLOCKS            28
#define I_FLAGS             32
#define I_BLOCK             40      /* i_block[15] */

/* ---- directory entry field offsets ------------------------------------ */
#define DE_INODE             0
#define DE_REC_LEN           4
#define DE_NAME_LEN          6
#define DE_FILE_TYPE         7
#define DE_NAME              8
#define DE_MIN               8      /* header size before the name */

/* ---- little-endian accessors ------------------------------------------ */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ---- image geometry --------------------------------------------------- */
static uint8_t *sb_ptr(ext2_fs_t *fs)
{
    return fs->img + EXT2_SUPER_OFF;
}

/*
 * A stand-in for the wall clock, which this kernel does not have.
 *
 * It exists for one field: i_dtime.  ext2 threads its list of orphaned inodes
 * through i_dtime, storing the *next inode number* there, so e2fsck reads any
 * small dtime as a link in that list rather than as a deletion time.  Marking
 * a freed inode with a plausible timestamp instead keeps the image clean; the
 * one mke2fs left in s_wtime is real, and no worse than any constant we could
 * invent.
 */
static uint32_t fs_now(ext2_fs_t *fs)
{
    uint32_t t = rd32(sb_ptr(fs) + SB_WTIME);
    return t ? t : 0x40000000u;          /* 2004, if the image had none */
}

/* Pointer to a whole block, or NULL if it falls outside the image. */
static uint8_t *blk_ptr(ext2_fs_t *fs, uint32_t blk)
{
    if (!blk || blk >= fs->blocks_count)
        return NULL;

    uint64_t off = (uint64_t)blk * fs->block_size;
    if (off + fs->block_size > fs->img_size)
        return NULL;
    return fs->img + off;
}

static void zero_block(ext2_fs_t *fs, uint32_t blk)
{
    uint8_t *p = blk_ptr(fs, blk);
    if (p)
        memset(p, 0, fs->block_size);
}

static uint8_t *gd_ptr(ext2_fs_t *fs, uint32_t group)
{
    if (group >= fs->group_count)
        return NULL;

    uint64_t off = (uint64_t)fs->gdt_off + (uint64_t)group * GD_SIZE;
    if (off + GD_SIZE > fs->img_size)
        return NULL;
    return fs->img + off;
}

/* Pointer to the raw inode, or NULL if the number is out of range. */
static uint8_t *inode_ptr(ext2_fs_t *fs, uint32_t ino)
{
    if (ino < 1 || ino > fs->inodes_count)
        return NULL;

    uint32_t g   = (ino - 1) / fs->inodes_per_group;
    uint32_t idx = (ino - 1) % fs->inodes_per_group;

    uint8_t *gd = gd_ptr(fs, g);
    if (!gd)
        return NULL;

    uint64_t off = (uint64_t)rd32(gd + GD_INODE_TABLE) * fs->block_size +
                   (uint64_t)idx * fs->inode_size;
    if (off + fs->inode_size > fs->img_size)
        return NULL;
    return fs->img + off;
}

/* How many blocks group `g` actually covers (the last one is usually short). */
static uint32_t group_blocks(ext2_fs_t *fs, uint32_t g)
{
    uint32_t base = fs->first_data_block + g * fs->blocks_per_group;
    if (base >= fs->blocks_count)
        return 0;

    uint32_t left = fs->blocks_count - base;
    return left < fs->blocks_per_group ? left : fs->blocks_per_group;
}

/* ---- bitmaps ---------------------------------------------------------- */
static int bit_test(const uint8_t *bm, uint32_t i)
{
    return (bm[i >> 3] >> (i & 7)) & 1;
}

static void bit_set(uint8_t *bm, uint32_t i)
{
    bm[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static void bit_clear(uint8_t *bm, uint32_t i)
{
    bm[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static void sb_add_free_blocks(ext2_fs_t *fs, int32_t delta)
{
    uint8_t *sb = sb_ptr(fs);
    wr32(sb + SB_FREE_BLOCKS, rd32(sb + SB_FREE_BLOCKS) + (uint32_t)delta);
}

static void sb_add_free_inodes(ext2_fs_t *fs, int32_t delta)
{
    uint8_t *sb = sb_ptr(fs);
    wr32(sb + SB_FREE_INODES, rd32(sb + SB_FREE_INODES) + (uint32_t)delta);
}

static void gd_add16(uint8_t *gd, int off, int32_t delta)
{
    wr16(gd + off, (uint16_t)(rd16(gd + off) + (uint16_t)delta));
}

/*
 * Grab one free data block.  First fit, resuming from wherever the previous
 * search stopped so that a long sequence of writes does not rescan the same
 * full groups over and over.
 */
static uint32_t balloc(ext2_fs_t *fs)
{
    uint32_t start = fs->alloc_hint / (fs->blocks_per_group ? fs->blocks_per_group : 1);
    if (start >= fs->group_count)
        start = 0;

    for (uint32_t n = 0; n < fs->group_count; n++) {
        uint32_t g  = (start + n) % fs->group_count;
        uint8_t *gd = gd_ptr(fs, g);
        if (!gd || rd16(gd + GD_FREE_BLOCKS) == 0)
            continue;

        uint8_t *bm = blk_ptr(fs, rd32(gd + GD_BLOCK_BITMAP));
        if (!bm)
            continue;

        uint32_t count = group_blocks(fs, g);
        for (uint32_t i = 0; i < count; i++) {
            if (bit_test(bm, i))
                continue;

            bit_set(bm, i);
            gd_add16(gd, GD_FREE_BLOCKS, -1);
            sb_add_free_blocks(fs, -1);

            uint32_t blk = fs->first_data_block + g * fs->blocks_per_group + i;
            fs->alloc_hint = blk;
            zero_block(fs, blk);
            return blk;
        }
    }
    return 0;
}

static void bfree(ext2_fs_t *fs, uint32_t blk)
{
    if (blk < fs->first_data_block || blk >= fs->blocks_count)
        return;

    uint32_t rel = blk - fs->first_data_block;
    uint32_t g   = rel / fs->blocks_per_group;
    uint32_t i   = rel % fs->blocks_per_group;

    uint8_t *gd = gd_ptr(fs, g);
    if (!gd)
        return;

    uint8_t *bm = blk_ptr(fs, rd32(gd + GD_BLOCK_BITMAP));
    if (!bm || !bit_test(bm, i))
        return;

    bit_clear(bm, i);
    gd_add16(gd, GD_FREE_BLOCKS, +1);
    sb_add_free_blocks(fs, +1);

    if (blk < fs->alloc_hint)
        fs->alloc_hint = blk;
}

/* Grab one free inode.  `isdir` only affects the per-group directory tally,
 * which the allocator uses to spread directories across groups; we keep it
 * accurate so the image still passes e2fsck. */
static uint32_t ialloc(ext2_fs_t *fs, int isdir)
{
    for (uint32_t g = 0; g < fs->group_count; g++) {
        uint8_t *gd = gd_ptr(fs, g);
        if (!gd || rd16(gd + GD_FREE_INODES) == 0)
            continue;

        uint8_t *bm = blk_ptr(fs, rd32(gd + GD_INODE_BITMAP));
        if (!bm)
            continue;

        for (uint32_t i = 0; i < fs->inodes_per_group; i++) {
            uint32_t ino = g * fs->inodes_per_group + i + 1;
            if (ino > fs->inodes_count)
                break;
            if (ino < fs->first_ino || bit_test(bm, i))
                continue;

            bit_set(bm, i);
            gd_add16(gd, GD_FREE_INODES, -1);
            sb_add_free_inodes(fs, -1);
            if (isdir)
                gd_add16(gd, GD_USED_DIRS, +1);

            uint8_t *ip = inode_ptr(fs, ino);
            if (ip)
                memset(ip, 0, fs->inode_size);
            return ino;
        }
    }
    return 0;
}

static void ifree(ext2_fs_t *fs, uint32_t ino, int isdir)
{
    if (ino < fs->first_ino || ino > fs->inodes_count)
        return;

    uint32_t g = (ino - 1) / fs->inodes_per_group;
    uint32_t i = (ino - 1) % fs->inodes_per_group;

    uint8_t *gd = gd_ptr(fs, g);
    if (!gd)
        return;

    uint8_t *bm = blk_ptr(fs, rd32(gd + GD_INODE_BITMAP));
    if (!bm || !bit_test(bm, i))
        return;

    bit_clear(bm, i);
    gd_add16(gd, GD_FREE_INODES, +1);
    sb_add_free_inodes(fs, +1);
    if (isdir)
        gd_add16(gd, GD_USED_DIRS, -1);
}

/* ---- block mapping ---------------------------------------------------- */
/*
 * Read the block number out of a 4-byte slot, creating the block when asked.
 * Every level of the indirect tree -- and i_block[] itself -- is just such a
 * slot, so this one helper covers all of them.
 */
static uint32_t slot_get(ext2_fs_t *fs, uint8_t *slot, int alloc, uint32_t *added)
{
    uint32_t b = rd32(slot);
    if (b || !alloc)
        return b;

    b = balloc(fs);
    if (!b)
        return 0;

    wr32(slot, b);
    if (added)
        (*added)++;
    return b;
}

/*
 * Translate a file-relative block index into an image block number.  With
 * `alloc` set, missing blocks (including the indirect blocks along the way)
 * are created; `added` accumulates how many, so the caller can keep i_blocks
 * truthful.
 */
static uint32_t bmap(ext2_fs_t *fs, uint8_t *ip, uint32_t iblk,
                     int alloc, uint32_t *added)
{
    uint32_t ppb = fs->block_size / 4;      /* pointers per indirect block */

    if (iblk < EXT2_NDIR_BLOCKS)
        return slot_get(fs, ip + I_BLOCK + iblk * 4, alloc, added);
    iblk -= EXT2_NDIR_BLOCKS;

    /* single indirect */
    if (iblk < ppb) {
        uint32_t ind = slot_get(fs, ip + I_BLOCK + EXT2_IND_BLOCK * 4, alloc, added);
        uint8_t *p   = blk_ptr(fs, ind);
        if (!p)
            return 0;
        return slot_get(fs, p + iblk * 4, alloc, added);
    }
    iblk -= ppb;

    /* double indirect */
    if (iblk < ppb * ppb) {
        uint32_t d = slot_get(fs, ip + I_BLOCK + EXT2_DIND_BLOCK * 4, alloc, added);
        uint8_t *p = blk_ptr(fs, d);
        if (!p)
            return 0;

        uint32_t ind = slot_get(fs, p + (iblk / ppb) * 4, alloc, added);
        uint8_t *q   = blk_ptr(fs, ind);
        if (!q)
            return 0;
        return slot_get(fs, q + (iblk % ppb) * 4, alloc, added);
    }
    iblk -= ppb * ppb;

    /* triple indirect */
    if (ppb && iblk / ppb < ppb * ppb) {
        uint32_t t = slot_get(fs, ip + I_BLOCK + EXT2_TIND_BLOCK * 4, alloc, added);
        uint8_t *p = blk_ptr(fs, t);
        if (!p)
            return 0;

        uint32_t d = slot_get(fs, p + (iblk / (ppb * ppb)) * 4, alloc, added);
        uint8_t *q = blk_ptr(fs, d);
        if (!q)
            return 0;

        uint32_t ind = slot_get(fs, q + ((iblk / ppb) % ppb) * 4, alloc, added);
        uint8_t *r   = blk_ptr(fs, ind);
        if (!r)
            return 0;
        return slot_get(fs, r + (iblk % ppb) * 4, alloc, added);
    }

    return 0;
}

/* Recursively release an indirect subtree.  level 0 is a plain data block. */
static void free_tree(ext2_fs_t *fs, uint32_t blk, int level)
{
    if (!blk)
        return;

    if (level > 0) {
        uint8_t *p = blk_ptr(fs, blk);
        if (p) {
            uint32_t ppb = fs->block_size / 4;
            for (uint32_t i = 0; i < ppb; i++)
                free_tree(fs, rd32(p + i * 4), level - 1);
        }
    }
    bfree(fs, blk);
}

static void inode_free_blocks(ext2_fs_t *fs, uint8_t *ip)
{
    for (int i = 0; i < EXT2_NDIR_BLOCKS; i++)
        free_tree(fs, rd32(ip + I_BLOCK + i * 4), 0);

    free_tree(fs, rd32(ip + I_BLOCK + EXT2_IND_BLOCK * 4), 1);
    free_tree(fs, rd32(ip + I_BLOCK + EXT2_DIND_BLOCK * 4), 2);
    free_tree(fs, rd32(ip + I_BLOCK + EXT2_TIND_BLOCK * 4), 3);

    memset(ip + I_BLOCK, 0, EXT2_N_BLOCKS * 4);
    wr32(ip + I_BLOCKS, 0);
}

static void inode_add_blocks(ext2_fs_t *fs, uint8_t *ip, uint32_t added)
{
    if (!added)
        return;

    /* i_blocks counts 512-byte sectors, not filesystem blocks. */
    uint32_t per = fs->block_size / 512;
    wr32(ip + I_BLOCKS, rd32(ip + I_BLOCKS) + added * per);
}

/* ---- mount ------------------------------------------------------------ */
int ext2_mount(ext2_fs_t *fs, uint8_t *img, uint32_t img_size)
{
    memset(fs, 0, sizeof(*fs));

    if (!img || img_size < EXT2_SUPER_OFF + 1024)
        return 0;

    fs->img      = img;
    fs->img_size = img_size;

    uint8_t *sb = sb_ptr(fs);
    if (rd16(sb + SB_MAGIC) != EXT2_MAGIC)
        return 0;

    /*
     * 1, 2 or 4 KiB.  Linux never uses a block larger than a page on x86, and
     * a directory entry's rec_len is a uint16 -- a 64 KiB block could not even
     * hold one entry spanning it -- so anything bigger is refused.
     */
    uint32_t log = rd32(sb + SB_LOG_BLOCK_SIZE);
    if (log > 2)
        return 0;
    fs->block_size = 1024u << log;

    fs->inodes_count     = rd32(sb + SB_INODES_COUNT);
    fs->blocks_count     = rd32(sb + SB_BLOCKS_COUNT);
    fs->first_data_block = rd32(sb + SB_FIRST_DATA_BLOCK);
    fs->blocks_per_group = rd32(sb + SB_BLOCKS_PER_GROUP);
    fs->inodes_per_group = rd32(sb + SB_INODES_PER_GROUP);

    if (!fs->blocks_per_group || !fs->inodes_per_group ||
        !fs->inodes_count || !fs->blocks_count)
        return 0;

    /*
     * Revision 0 predates the variable inode size and the reserved-inode
     * count, so those two fields are absent and take their historic values.
     */
    if (rd32(sb + SB_REV_LEVEL) == 0) {
        fs->inode_size = EXT2_GOOD_OLD_ISIZE;
        fs->first_ino  = 11;
    } else {
        fs->inode_size = rd16(sb + SB_INODE_SIZE);
        fs->first_ino  = rd32(sb + SB_FIRST_INO);
    }
    if (fs->inode_size < EXT2_GOOD_OLD_ISIZE || fs->inode_size > fs->block_size)
        return 0;
    if (fs->first_ino < 2)
        return 0;

    /*
     * Incompatible features are exactly the ones we may not ignore.  We
     * understand FILETYPE and nothing else, so anything further -- extents,
     * 64-bit, meta_bg, a journal awaiting recovery -- means the image would be
     * misread, and refusing is the only safe answer.  Compatible and
     * read-only-compatible bits (has_journal, sparse_super, large_file...) are
     * ignorable by definition and are ignored.
     */
    uint32_t incompat = rd32(sb + SB_FEATURE_INCOMPAT);
    if (incompat & ~(uint32_t)EXT2_FEATURE_INCOMPAT_FILETYPE)
        return 0;
    fs->has_filetype = (incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) ? 1 : 0;

    /* Groups cover everything from the first data block onwards. */
    uint32_t span = fs->blocks_count - fs->first_data_block;
    fs->group_count = (span + fs->blocks_per_group - 1) / fs->blocks_per_group;
    if (!fs->group_count)
        return 0;

    fs->gdt_off    = (fs->first_data_block + 1) * fs->block_size;
    fs->alloc_hint = fs->first_data_block;

    if ((uint64_t)fs->gdt_off + (uint64_t)fs->group_count * GD_SIZE > img_size)
        return 0;

    /* The root inode has to be there, and has to be a directory. */
    uint8_t *root = inode_ptr(fs, EXT2_ROOT_INO);
    if (!root || (rd16(root + I_MODE) & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return 0;

    return 1;
}

/* ---- reading ---------------------------------------------------------- */
int ext2_is_dir(const ext2_dirent_t *ent)
{
    return ent && (ent->mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
}

/* Fill a dirent from an inode number plus the name we found it under. */
static void ent_fill(ext2_fs_t *fs, ext2_dirent_t *out, uint32_t ino,
                     const char *name, uint32_t name_len)
{
    memset(out, 0, sizeof(*out));
    out->ino = ino;

    uint32_t n = name_len;
    if (n > EXT2_NAME_MAX - 1)
        n = EXT2_NAME_MAX - 1;
    for (uint32_t i = 0; i < n; i++)
        out->name[i] = name[i];
    out->name[n] = 0;

    uint8_t *ip = inode_ptr(fs, ino);
    if (ip) {
        out->mode = rd16(ip + I_MODE);
        out->size = rd32(ip + I_SIZE);
    }
}

uint32_t ext2_read(ext2_fs_t *fs, const ext2_dirent_t *ent,
                   uint32_t off, void *buf, uint32_t len)
{
    if (!fs || !ent || !ent->ino || !buf)
        return 0;

    uint8_t *ip = inode_ptr(fs, ent->ino);
    if (!ip)
        return 0;

    uint32_t size = rd32(ip + I_SIZE);
    if (off >= size)
        return 0;
    if (len > size - off)
        len = size - off;

    uint8_t *d    = (uint8_t *)buf;
    uint32_t done = 0;

    while (done < len) {
        uint32_t iblk = (off + done) / fs->block_size;
        uint32_t skip = (off + done) % fs->block_size;
        uint32_t take = fs->block_size - skip;
        if (take > len - done)
            take = len - done;

        uint32_t blk = bmap(fs, ip, iblk, 0, NULL);
        uint8_t *src = blk ? blk_ptr(fs, blk) : NULL;

        /* A hole reads as zeroes, which is exactly what a sparse file means. */
        if (src)
            memcpy(d + done, src + skip, take);
        else
            memset(d + done, 0, take);

        done += take;
    }
    return done;
}

/* ---- directory iteration ---------------------------------------------- */
void ext2_opendir(ext2_fs_t *fs, uint32_t ino, ext2_dir_t *dir)
{
    if (!ino)
        ino = EXT2_ROOT_INO;

    dir->fs   = fs;
    dir->ino  = ino;
    dir->off  = 0;
    dir->size = 0;

    uint8_t *ip = inode_ptr(fs, ino);
    if (ip && (rd16(ip + I_MODE) & EXT2_S_IFMT) == EXT2_S_IFDIR)
        dir->size = rd32(ip + I_SIZE);
}

/* Pointer to the raw entry at byte offset `off` of directory `ip`. */
static uint8_t *de_at(ext2_fs_t *fs, uint8_t *ip, uint32_t off)
{
    uint32_t blk = bmap(fs, ip, off / fs->block_size, 0, NULL);
    uint8_t *p   = blk_ptr(fs, blk);
    if (!p)
        return NULL;
    return p + (off % fs->block_size);
}

static int name_is_dot(const uint8_t *de)
{
    uint32_t len = de[DE_NAME_LEN];
    if (len == 1 && de[DE_NAME] == '.')
        return 1;
    if (len == 2 && de[DE_NAME] == '.' && de[DE_NAME + 1] == '.')
        return 1;
    return 0;
}

int ext2_readdir(ext2_dir_t *dir, ext2_dirent_t *out)
{
    if (!dir || !dir->fs || !dir->ino)
        return 0;

    ext2_fs_t *fs = dir->fs;
    uint8_t   *ip = inode_ptr(fs, dir->ino);
    if (!ip)
        return 0;

    while (dir->off + DE_MIN <= dir->size) {
        uint8_t *de = de_at(fs, ip, dir->off);
        if (!de)
            return 0;

        uint32_t rec = rd16(de + DE_REC_LEN);
        /* rec_len is what carries us forward; a bad one would loop forever. */
        if (rec < DE_MIN || (rec & 3) || dir->off + rec > dir->size)
            return 0;

        uint32_t ino = rd32(de + DE_INODE);
        dir->off += rec;

        /* inode 0 marks a hole left by a deletion; "." and ".." are noise
         * for our callers, who all want the contents rather than the links. */
        if (ino && !name_is_dot(de)) {
            ent_fill(fs, out, ino, (const char *)(de + DE_NAME), de[DE_NAME_LEN]);
            return 1;
        }
    }
    return 0;
}

/* The inode a directory's ".." entry points at.  getdents64 has to emit ".."
 * (with the parent inode number) even though ext2_readdir skips it, so this
 * walks the raw directory blocks the same way dir_find does but looks for the
 * ".." name specifically.  The root directory's parent is itself. */
uint32_t ext2_parent_ino(ext2_fs_t *fs, uint32_t ino)
{
    if (ino == EXT2_ROOT_INO || !ino)
        return EXT2_ROOT_INO;

    uint8_t *ip = inode_ptr(fs, ino);
    if (!ip || (rd16(ip + I_MODE) & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return 0;

    uint32_t size = rd32(ip + I_SIZE);
    uint32_t off  = 0;
    while (off + DE_MIN <= size) {
        uint8_t *de = de_at(fs, ip, off);
        if (!de)
            return 0;

        uint32_t rec = rd16(de + DE_REC_LEN);
        if (rec < DE_MIN || (rec & 3) || off + rec > size)
            return 0;

        uint32_t ino2 = rd32(de + DE_INODE);
        if (de[DE_NAME_LEN] == 2 && de[DE_NAME] == '.' &&
            de[DE_NAME + 1] == '.' && ino2)
            return ino2;

        off += rec;
    }
    return 0;
}

/* ---- name lookup ------------------------------------------------------ */
static uint32_t comp_len(const char *p)
{
    uint32_t n = 0;
    while (p[n] && p[n] != '/')
        n++;
    return n;
}

/* Find `name` (length `len`) in directory `dino`.  Returns the inode, or 0. */
static uint32_t dir_find(ext2_fs_t *fs, uint32_t dino,
                         const char *name, uint32_t len)
{
    uint8_t *ip = inode_ptr(fs, dino);
    if (!ip || (rd16(ip + I_MODE) & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return 0;

    uint32_t size = rd32(ip + I_SIZE);
    uint32_t off  = 0;

    while (off + DE_MIN <= size) {
        uint8_t *de = de_at(fs, ip, off);
        if (!de)
            return 0;

        uint32_t rec = rd16(de + DE_REC_LEN);
        if (rec < DE_MIN || (rec & 3) || off + rec > size)
            return 0;

        uint32_t ino = rd32(de + DE_INODE);
        if (ino && de[DE_NAME_LEN] == len &&
            memcmp(de + DE_NAME, name, len) == 0)
            return ino;

        off += rec;
    }
    return 0;
}

/* ---- symbolic links --------------------------------------------------- */
#define SYMLINK_MAX  4096          /* longest target we will store or follow */
#define PW_MAXCOMP   64            /* longest path, in components */
#define PW_LOOP      8             /* max symlink expansions per lookup */

/*
 * Read a symlink's target into buf (cap bytes).  Returns the length, or a
 * negative EXT2_* code.  The target lives in an ordinary data block written by
 * ext2_symlink via ext2_write, so ext2_read fetches it.
 */
static int read_symlink_target(ext2_fs_t *fs, uint32_t ino, char *buf, uint32_t cap)
{
    uint8_t *ip = inode_ptr(fs, ino);
    if (!ip)
        return EXT2_EINVAL;
    if ((rd16(ip + I_MODE) & EXT2_S_IFMT) != EXT2_S_IFLNK)
        return EXT2_EINVAL;

    uint32_t size = rd32(ip + I_SIZE);
    uint32_t n    = size < cap ? size : cap - 1;

    ext2_dirent_t ent;
    ent.ino  = ino;
    ent.size = size;
    ent.mode = rd16(ip + I_MODE);

    uint32_t got = ext2_read(fs, &ent, 0, buf, n);
    buf[got] = '\0';
    return (int)got;
}

/*
 * Walk an absolute path to its final component, following symlinks along the
 * way.  The caller hands over a *mutable* copy of the path: when a symlink is
 * expanded we splice its target into the component list and re-walk.  `follow`
 * controls whether a symlink at the very end is itself returned (0, for
 * lstat/readlink/unlink) or resolved through to its target (1, for open/stat).
 *
 * On success `parent_out`/`leaf_out` describe the *literal* final name (so a
 * creator/unlinker can place or remove it), and the return value is the
 * resolved inode (the target's, when the final symlink was followed).
 */
static uint32_t path_walk(ext2_fs_t *fs, char *path, int follow,
                          uint32_t *parent_out, char *leaf_buf,
                          uint32_t *leaf_len_out)
{
    if (!path || path[0] != '/')
        return 0;

    char comp[PW_MAXCOMP][EXT2_NAME_MAX];
    int  ncomp = 0;

    {   /* split the path into components, skipping empty runs of slashes */
        const char *p = path;
        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;
            if (ncomp >= PW_MAXCOMP) return 0;
            uint32_t l = 0;
            while (*p && *p != '/') {
                if (l < EXT2_NAME_MAX - 1)
                    comp[ncomp][l++] = *p;
                p++;
            }
            comp[ncomp][l] = '\0';
            ncomp++;
        }
    }

    if (ncomp == 0) {                 /* "/" resolves to the root itself */
        if (parent_out) *parent_out = EXT2_ROOT_INO;
        if (leaf_buf)   leaf_buf[0] = '\0';
        if (leaf_len_out) *leaf_len_out = 0;
        return EXT2_ROOT_INO;
    }

    uint32_t cwd    = EXT2_ROOT_INO;
    uint32_t cur    = EXT2_ROOT_INO;
    uint32_t parent = cwd;

    for (int i = 0; i < ncomp; i++) {
        uint32_t ino = dir_find(fs, cwd, comp[i], (uint32_t)strlen(comp[i]));
        if (!ino) {
            if (i != ncomp - 1)
                return 0;             /* a missing intermediate name */
            parent = cwd;
            cur    = 0;                /* the final name does not exist */
            goto found;
        }

        uint8_t *ip = inode_ptr(fs, ino);
        if (!ip)
            return 0;
        uint16_t mt = rd16(ip + I_MODE) & EXT2_S_IFMT;

        if (mt == EXT2_S_IFLNK) {
            if (i == ncomp - 1 && !follow) {
                parent = cwd;         /* the symlink name itself */
                cur    = ino;
                goto found;
            }
            /* Expand the target into the component list and re-walk. */
            if (i >= PW_LOOP)
                return 0;             /* too many nested symlinks */
            char target[SYMLINK_MAX];
            int  tlen = read_symlink_target(fs, ino, target, sizeof target);
            if (tlen < 0)
                return 0;

            char tcomp[PW_MAXCOMP][EXT2_NAME_MAX];
            int  nt = 0;
            {
                const char *p = target;
                while (*p) {
                    while (*p == '/') p++;
                    if (!*p) break;
                    if (nt >= PW_MAXCOMP) return 0;
                    uint32_t l = 0;
                    while (*p && *p != '/') {
                        if (l < EXT2_NAME_MAX - 1)
                            tcomp[nt][l++] = *p;
                        p++;
                    }
                    tcomp[nt][l] = '\0';
                    nt++;
                }
            }

            /* Rebuild the component list with the symlink spliced in.
             * A relative target keeps the components before the symlink
             * (save[0..i-1]) and after it (save[i+1..]); an absolute target
             * is a complete path of its own and must DISCARD everything
             * before the symlink, or we would end up with e.g.
             * "/a/b" -> "/c" walking as "/a/c". */
            int  is_abs = (target[0] == '/');
            char save[PW_MAXCOMP][EXT2_NAME_MAX];
            memcpy(save, comp, sizeof save);
            int sn = ncomp, k = 0;
            if (!is_abs)
                for (int j = 0; j < i; j++) { strncpy(comp[k], save[j], EXT2_NAME_MAX - 1); comp[k][EXT2_NAME_MAX - 1] = '\0'; k++; }
            for (int j = 0; j < nt; j++) { strncpy(comp[k], tcomp[j], EXT2_NAME_MAX - 1); comp[k][EXT2_NAME_MAX - 1] = '\0'; k++; }
            if (!is_abs)
                for (int j = i + 1; j < sn; j++) { strncpy(comp[k], save[j], EXT2_NAME_MAX - 1); comp[k][EXT2_NAME_MAX - 1] = '\0'; k++; }
            ncomp = k;

            if (is_abs) {                  /* absolute: restart from root */
                cwd = EXT2_ROOT_INO;
                i   = -1;
            } else {                       /* relative: continue in cwd */
                i   = i - 1;
            }
            continue;
        }

        if (mt == EXT2_S_IFDIR) {
            parent = cwd;
            cwd    = ino;
            cur    = ino;
        } else {
            if (i != ncomp - 1)
                return 0;                 /* a non-directory mid-path */
            parent = cwd;
            cur    = ino;
            goto found;
        }
    }

found:
    if (parent_out)   *parent_out = parent;
    if (leaf_buf) {
        strncpy(leaf_buf, comp[ncomp - 1], EXT2_NAME_MAX - 1);
        leaf_buf[EXT2_NAME_MAX - 1] = '\0';
    }
    if (leaf_len_out) *leaf_len_out = (uint32_t)strlen(comp[ncomp - 1]);
    return cur;
}

int ext2_lookup(ext2_fs_t *fs, const char *path, ext2_dirent_t *out,
                int follow_final)
{
    if (!fs || !path || path[0] != '/')
        return 0;

    /* path_walk mutates its argument (it splices symlink targets in), so we
     * hand it a private copy rather than the caller's string. */
    char pb[SYMLINK_MAX];
    strncpy(pb, path, sizeof pb - 1);
    pb[sizeof pb - 1] = '\0';

    char        leaf[EXT2_NAME_MAX];
    uint32_t    leaf_len = 0;
    uint32_t    ino      = path_walk(fs, pb, follow_final, NULL, leaf, &leaf_len);

    if (!ino)
        return 0;

    if (out) {
        if (leaf_len)
            ent_fill(fs, out, ino, leaf, leaf_len);
        else
            ent_fill(fs, out, ino, "/", 1);      /* the root itself */
    }
    return 1;
}

/* ---- writing ---------------------------------------------------------- */
uint32_t ext2_write(ext2_fs_t *fs, ext2_dirent_t *ent,
                    uint32_t off, const void *buf, uint32_t len)
{
    if (!fs || !ent || !ent->ino || !buf || !len)
        return 0;

    uint8_t *ip = inode_ptr(fs, ent->ino);
    if (!ip)
        return 0;
    if ((rd16(ip + I_MODE) & EXT2_S_IFMT) == EXT2_S_IFDIR)
        return 0;

    const uint8_t *s     = (const uint8_t *)buf;
    uint32_t       done  = 0;
    uint32_t       added = 0;

    while (done < len) {
        uint32_t iblk = (off + done) / fs->block_size;
        uint32_t skip = (off + done) % fs->block_size;
        uint32_t take = fs->block_size - skip;
        if (take > len - done)
            take = len - done;

        uint32_t blk = bmap(fs, ip, iblk, 1, &added);
        uint8_t *dst = blk ? blk_ptr(fs, blk) : NULL;
        if (!dst)
            break;                              /* the volume is full */

        memcpy(dst + skip, s + done, take);
        done += take;
    }

    inode_add_blocks(fs, ip, added);

    uint32_t end = off + done;
    if (end > rd32(ip + I_SIZE))
        wr32(ip + I_SIZE, end);

    ent->size = rd32(ip + I_SIZE);
    return done;
}

int ext2_truncate(ext2_fs_t *fs, ext2_dirent_t *ent)
{
    if (!fs || !ent || !ent->ino)
        return EXT2_EINVAL;

    uint8_t *ip = inode_ptr(fs, ent->ino);
    if (!ip)
        return EXT2_ENOENT;
    if ((rd16(ip + I_MODE) & EXT2_S_IFMT) == EXT2_S_IFDIR)
        return EXT2_EINVAL;

    inode_free_blocks(fs, ip);
    wr32(ip + I_SIZE, 0);
    ent->size = 0;
    return EXT2_OK;
}

/*
 * Replace the permission bits of an inode, leaving the file-type bits alone.
 * Only the low 12 bits (rwx, setuid/setgid/sticky) are the caller's to set;
 * clobbering EXT2_S_IFMT would turn a directory into a regular file as far as
 * every later lookup is concerned, so we mask it off rather than trust it.
 */
int ext2_chmod(ext2_fs_t *fs, ext2_dirent_t *ent, uint16_t mode)
{
    if (!fs || !ent || !ent->ino)
        return EXT2_EINVAL;

    uint8_t *ip = inode_ptr(fs, ent->ino);
    if (!ip)
        return EXT2_ENOENT;

    uint16_t cur = rd16(ip + I_MODE);
    uint16_t new = (uint16_t)((cur & EXT2_S_IFMT) | (mode & 0x0FFF));

    wr16(ip + I_MODE, new);
    wr32(ip + I_CTIME, fs_now(fs));
    ent->mode = new;
    return EXT2_OK;
}

/* ---- directory mutation ----------------------------------------------- */
/* Fill in an entry's inode, name and type.  rec_len belongs to the caller,
 * who is the only one who knows how much room the slot really spans. */
static void put_entry(ext2_fs_t *fs, uint8_t *de, uint32_t ino,
                      const char *name, uint32_t len, uint8_t ft)
{
    wr32(de + DE_INODE, ino);
    de[DE_NAME_LEN]  = (uint8_t)len;
    de[DE_FILE_TYPE] = fs->has_filetype ? ft : 0;
    memcpy(de + DE_NAME, name, len);
}

/* Map an inode mode to a directory-entry file_type byte. */
static uint8_t mode_to_ft(uint16_t mode)
{
    switch (mode & EXT2_S_IFMT) {
    case EXT2_S_IFDIR:  return EXT2_FT_DIR;
    case EXT2_S_IFLNK:  return EXT2_FT_SYMLINK;
    default:            return EXT2_FT_REG;
    }
}

/* Space an entry with this name actually occupies, rounded as ext2 requires. */
static uint32_t ent_need(uint32_t name_len)
{
    return (DE_MIN + name_len + 3) & ~3u;
}

/*
 * Link `name` to `ino` inside directory `dino`.
 *
 * ext2 has no free list for directory entries: the slack at the end of each
 * entry's rec_len *is* the free space.  So adding a name means finding an
 * entry whose rec_len is bigger than the entry needs, shrinking it to its
 * true size, and building the new entry in the remainder.
 */
static int dir_add(ext2_fs_t *fs, uint32_t dino, const char *name,
                   uint32_t len, uint32_t ino, uint16_t child_mode)
{
    if (!len || len > 255)
        return EXT2_EINVAL;

    uint8_t *dp = inode_ptr(fs, dino);
    if (!dp)
        return EXT2_ENOENT;
    if ((rd16(dp + I_MODE) & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return EXT2_ENOTDIR;

    uint8_t ft = mode_to_ft(child_mode);

    uint32_t need = ent_need(len);
    uint32_t size = rd32(dp + I_SIZE);
    uint32_t off  = 0;

    while (off + DE_MIN <= size) {
        uint8_t *de = de_at(fs, dp, off);
        if (!de)
            break;

        uint32_t rec = rd16(de + DE_REC_LEN);
        if (rec < DE_MIN || (rec & 3) || off + rec > size)
            break;

        uint32_t cur  = rd32(de + DE_INODE);
        uint32_t used = cur ? ent_need(de[DE_NAME_LEN]) : 0;

        if (rec - used >= need) {
            uint8_t *slot = de;
            if (used) {                          /* split the live entry */
                wr16(de + DE_REC_LEN, (uint16_t)used);
                slot = de + used;
                wr16(slot + DE_REC_LEN, (uint16_t)(rec - used));
            }
            put_entry(fs, slot, ino, name, len, ft);
            return EXT2_OK;
        }
        off += rec;
    }

    /* Every block is packed: give the directory one more. */
    uint32_t added = 0;
    uint32_t blk   = bmap(fs, dp, size / fs->block_size, 1, &added);
    uint8_t *b     = blk ? blk_ptr(fs, blk) : NULL;
    if (!b)
        return EXT2_ENOSPC;

    inode_add_blocks(fs, dp, added);
    memset(b, 0, fs->block_size);
    wr16(b + DE_REC_LEN, (uint16_t)fs->block_size);
    put_entry(fs, b, ino, name, len, ft);
    wr32(dp + I_SIZE, size + fs->block_size);
    return EXT2_OK;
}

/*
 * Unlink a name.  The entry is not erased -- it is swallowed by its
 * predecessor's rec_len, which is how ext2 turns it back into free space.
 * An entry that begins a block has no predecessor to merge with, so it is
 * merely marked unused by zeroing its inode number.
 */
static int dir_remove(ext2_fs_t *fs, uint32_t dino, const char *name, uint32_t len)
{
    uint8_t *dp = inode_ptr(fs, dino);
    if (!dp)
        return EXT2_ENOENT;
    if ((rd16(dp + I_MODE) & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return EXT2_ENOTDIR;

    uint32_t size     = rd32(dp + I_SIZE);
    uint32_t off      = 0;
    uint32_t prev_off = 0;
    int      have_prev = 0;

    while (off + DE_MIN <= size) {
        uint8_t *de = de_at(fs, dp, off);
        if (!de)
            break;

        uint32_t rec = rd16(de + DE_REC_LEN);
        if (rec < DE_MIN || (rec & 3) || off + rec > size)
            break;

        if (rd32(de + DE_INODE) && de[DE_NAME_LEN] == len &&
            memcmp(de + DE_NAME, name, len) == 0) {

            int same_block = have_prev &&
                             (prev_off / fs->block_size) == (off / fs->block_size);
            if (same_block) {
                uint8_t *pd = de_at(fs, dp, prev_off);
                if (pd)
                    wr16(pd + DE_REC_LEN,
                         (uint16_t)(rd16(pd + DE_REC_LEN) + rec));
            } else {
                wr32(de + DE_INODE, 0);
            }
            return EXT2_OK;
        }

        prev_off  = off;
        have_prev = 1;
        off      += rec;
    }
    return EXT2_ENOENT;
}

/* readdir already hides "." and "..", so an empty directory yields nothing. */
static int dir_is_empty(ext2_fs_t *fs, uint32_t dino)
{
    ext2_dir_t    d;
    ext2_dirent_t e;

    ext2_opendir(fs, dino, &d);
    return ext2_readdir(&d, &e) ? 0 : 1;
}

/* ---- create / unlink -------------------------------------------------- */
int ext2_create(ext2_fs_t *fs, const char *path, int isdir, ext2_dirent_t *out)
{
    if (!fs || !path || path[0] != '/')
        return EXT2_EINVAL;

    uint32_t    parent   = 0;
    char        leaf[EXT2_NAME_MAX];
    uint32_t    leaf_len = 0;

    char pb[SYMLINK_MAX];
    strncpy(pb, path, sizeof pb - 1);
    pb[sizeof pb - 1] = '\0';

    if (path_walk(fs, pb, 0, &parent, leaf, &leaf_len))
        return EXT2_EEXIST;
    if (!parent)
        return EXT2_ENOENT;              /* a directory along the way is missing */
    if (!leaf_len || leaf_len > 255)
        return EXT2_EINVAL;

    uint8_t *pp = inode_ptr(fs, parent);
    if (!pp)
        return EXT2_ENOENT;
    if ((rd16(pp + I_MODE) & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return EXT2_ENOTDIR;

    uint32_t ino = ialloc(fs, isdir);
    if (!ino)
        return EXT2_ENOSPC;

    uint8_t *ip = inode_ptr(fs, ino);
    if (!ip) {
        ifree(fs, ino, isdir);
        return EXT2_EINVAL;
    }

    memset(ip, 0, fs->inode_size);
    wr16(ip + I_MODE, (uint16_t)(isdir ? (EXT2_S_IFDIR | 0755)
                                       : (EXT2_S_IFREG | 0644)));
    wr16(ip + I_LINKS, (uint16_t)(isdir ? 2 : 1));   /* a dir links to itself */

    uint32_t now = fs_now(fs);
    wr32(ip + I_ATIME, now);
    wr32(ip + I_CTIME, now);
    wr32(ip + I_MTIME, now);

    if (isdir) {
        uint32_t added = 0;
        uint32_t blk   = bmap(fs, ip, 0, 1, &added);
        uint8_t *b     = blk ? blk_ptr(fs, blk) : NULL;
        if (!b) {
            inode_free_blocks(fs, ip);
            ifree(fs, ino, 1);
            return EXT2_ENOSPC;
        }

        inode_add_blocks(fs, ip, added);
        memset(b, 0, fs->block_size);

        /* "." takes the minimum 12 bytes; ".." spans the rest of the block. */
        wr16(b + DE_REC_LEN, 12);
        put_entry(fs, b, ino, ".", 1, 1);

        uint8_t *dd = b + 12;
        wr16(dd + DE_REC_LEN, (uint16_t)(fs->block_size - 12));
        put_entry(fs, dd, parent, "..", 2, 1);

        wr32(ip + I_SIZE, fs->block_size);
    }

    int r = dir_add(fs, parent, leaf, leaf_len, ino,
                    isdir ? (EXT2_S_IFDIR | 0755) : (EXT2_S_IFREG | 0644));
    if (r != EXT2_OK) {
        inode_free_blocks(fs, ip);
        ifree(fs, ino, isdir);
        return r;
    }

    /* The new directory's ".." is a second link to the parent. */
    if (isdir)
        wr16(pp + I_LINKS, (uint16_t)(rd16(pp + I_LINKS) + 1));

    if (out)
        ent_fill(fs, out, ino, leaf, leaf_len);
    return EXT2_OK;
}

int ext2_unlink(ext2_fs_t *fs, const char *path)
{
    if (!fs || !path || path[0] != '/')
        return EXT2_EINVAL;

    uint32_t    parent   = 0;
    char        leaf[EXT2_NAME_MAX];
    uint32_t    leaf_len = 0;

    char pb[SYMLINK_MAX];
    strncpy(pb, path, sizeof pb - 1);
    pb[sizeof pb - 1] = '\0';

    uint32_t    ino      = path_walk(fs, pb, 0, &parent, leaf, &leaf_len);

    if (!ino)
        return EXT2_ENOENT;
    if (!leaf_len || ino == EXT2_ROOT_INO)
        return EXT2_EINVAL;              /* "/" is not something we may remove */

    uint8_t *ip = inode_ptr(fs, ino);
    if (!ip)
        return EXT2_ENOENT;

    int isdir = (rd16(ip + I_MODE) & EXT2_S_IFMT) == EXT2_S_IFDIR;
    if (isdir && !dir_is_empty(fs, ino))
        return EXT2_ENOTEMPTY;

    int r = dir_remove(fs, parent, leaf, leaf_len);
    if (r != EXT2_OK)
        return r;

    if (isdir) {
        /* Losing the child costs the parent the link its ".." held. */
        uint8_t *pp = inode_ptr(fs, parent);
        if (pp && rd16(pp + I_LINKS) > 1)
            wr16(pp + I_LINKS, (uint16_t)(rd16(pp + I_LINKS) - 1));
        wr16(ip + I_LINKS, 0);
    } else {
        uint16_t links = rd16(ip + I_LINKS);
        if (links > 0)
            wr16(ip + I_LINKS, (uint16_t)(--links));
        if (links)
            return EXT2_OK;              /* another name still refers to it */
    }

    inode_free_blocks(fs, ip);
    wr32(ip + I_SIZE, 0);
    wr32(ip + I_DTIME, fs_now(fs));      /* a non-zero dtime means "deleted" */
    ifree(fs, ino, isdir);
    return EXT2_OK;
}

/* ---- symlinks ---------------------------------------------------------- */
int ext2_symlink(ext2_fs_t *fs, const char *target, const char *path)
{
    if (!fs || !target || !path || path[0] != '/')
        return EXT2_EINVAL;
    if (strlen(target) == 0 || strlen(target) >= SYMLINK_MAX)
        return EXT2_EINVAL;

    char pb[SYMLINK_MAX];
    strncpy(pb, path, sizeof pb - 1);
    pb[sizeof pb - 1] = '\0';

    uint32_t    parent   = 0;
    char        leaf[EXT2_NAME_MAX];
    uint32_t    leaf_len = 0;
    if (path_walk(fs, pb, 0, &parent, leaf, &leaf_len))
        return EXT2_EEXIST;              /* the name already exists */
    if (!parent)
        return EXT2_ENOENT;
    if (!leaf_len || leaf_len > 255)
        return EXT2_EINVAL;

    uint8_t *pp = inode_ptr(fs, parent);
    if (!pp)
        return EXT2_ENOENT;
    if ((rd16(pp + I_MODE) & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return EXT2_ENOTDIR;

    uint32_t ino = ialloc(fs, 0);
    if (!ino)
        return EXT2_ENOSPC;

    uint8_t *ip = inode_ptr(fs, ino);
    if (!ip) {
        ifree(fs, ino, 0);
        return EXT2_EINVAL;
    }

    memset(ip, 0, fs->inode_size);
    wr16(ip + I_MODE, (uint16_t)(EXT2_S_IFLNK | 0777));
    wr16(ip + I_LINKS, 1);
    uint32_t now = fs_now(fs);
    wr32(ip + I_ATIME, now);
    wr32(ip + I_CTIME, now);
    wr32(ip + I_MTIME, now);

    /* The target is stored in an ordinary data block via ext2_write, which
     * keeps i_size and i_blocks honest. */
    ext2_dirent_t ent;
    ent.ino  = ino;
    ent.size = 0;
    ent.mode = (uint16_t)(EXT2_S_IFLNK | 0777);
    strncpy(ent.name, leaf, EXT2_NAME_MAX - 1);
    ent.name[EXT2_NAME_MAX - 1] = '\0';

    uint32_t tlen = (uint32_t)strlen(target);
    uint32_t done = ext2_write(fs, &ent, 0, target, tlen);
    if (done != tlen) {
        inode_free_blocks(fs, ip);
        ifree(fs, ino, 0);
        return EXT2_ENOSPC;
    }
    wr32(ip + I_SIZE, tlen);

    int r = dir_add(fs, parent, leaf, leaf_len, ino,
                    (uint16_t)(EXT2_S_IFLNK | 0777));
    if (r != EXT2_OK) {
        inode_free_blocks(fs, ip);
        ifree(fs, ino, 0);
        return r;
    }
    return EXT2_OK;
}

int ext2_readlink(ext2_fs_t *fs, const char *path, char *buf, uint32_t cap)
{
    if (!fs || !buf || cap == 0)
        return EXT2_EINVAL;

    char pb[SYMLINK_MAX];
    strncpy(pb, path, sizeof pb - 1);
    pb[sizeof pb - 1] = '\0';

    uint32_t    parent   = 0;
    char        leaf[EXT2_NAME_MAX];
    uint32_t    leaf_len = 0;
    uint32_t    ino      = path_walk(fs, pb, 0, &parent, leaf, &leaf_len);
    if (!ino)
        return EXT2_ENOENT;

    return read_symlink_target(fs, ino, buf, cap);
}

/* ---- rename ------------------------------------------------------------ */
int ext2_rename(ext2_fs_t *fs, const char *src, const char *dst)
{
    if (!fs || !src || !dst || src[0] != '/' || dst[0] != '/')
        return EXT2_EINVAL;
    if (strcmp(src, dst) == 0)
        return EXT2_OK;

    /* Resolve both names literally (no trailing-symlink following): rename
     * operates on the source and destination names themselves. */
    char spb[SYMLINK_MAX];
    strncpy(spb, src, sizeof spb - 1); spb[sizeof spb - 1] = '\0';
    uint32_t sparent = 0; char sleaf[EXT2_NAME_MAX]; uint32_t sleaf_len = 0;
    uint32_t sino = path_walk(fs, spb, 0, &sparent, sleaf, &sleaf_len);
    if (!sino)
        return EXT2_ENOENT;
    if (sino == EXT2_ROOT_INO)
        return EXT2_EINVAL;

    uint8_t *sip = inode_ptr(fs, sino);
    if (!sip)
        return EXT2_ENOENT;
    uint16_t smode  = rd16(sip + I_MODE);
    int      sisdir = (smode & EXT2_S_IFMT) == EXT2_S_IFDIR;

    char dpb[SYMLINK_MAX];
    strncpy(dpb, dst, sizeof dpb - 1); dpb[sizeof dpb - 1] = '\0';
    uint32_t dparent = 0; char dleaf[EXT2_NAME_MAX]; uint32_t dleaf_len = 0;
    uint32_t dino = path_walk(fs, dpb, 0, &dparent, dleaf, &dleaf_len);
    if (dino == EXT2_ROOT_INO)
        return EXT2_EINVAL;
    if (!dparent || !dleaf_len)
        return EXT2_EINVAL;

    /* A directory cannot be renamed into itself or one of its children. */
    if (sisdir && dparent) {
        size_t sl = strlen(src);
        if (strncmp(dst, src, sl) == 0 && (dst[sl] == '/' || dst[sl] == '\0'))
            return EXT2_EINVAL;
    }

    uint8_t *dip = dino ? inode_ptr(fs, dino) : NULL;
    int      disdir = dip && (rd16(dip + I_MODE) & EXT2_S_IFMT) == EXT2_S_IFDIR;

    /* Type mismatches POSIX rejects: dir over file, file over dir. */
    if (sisdir && dip && !disdir)
        return EXT2_ENOTDIR;
    if (!sisdir && dip && disdir)
        return EXT2_EISDIR;

    if (sisdir && dip && disdir && !dir_is_empty(fs, dino))
        return EXT2_ENOTEMPTY;

    /* If the destination already existed, drop its old name and (because it
     * had exactly one link) free its inode. */
    if (dino) {
        int r = dir_remove(fs, dparent, dleaf, dleaf_len);
        if (r != EXT2_OK)
            return r;
        if (disdir) {
            uint8_t *pp = inode_ptr(fs, dparent);
            if (pp && rd16(pp + I_LINKS) > 1)
                wr16(pp + I_LINKS, (uint16_t)(rd16(pp + I_LINKS) - 1));
            wr16(dip + I_LINKS, 0);
            inode_free_blocks(fs, dip);
            wr32(dip + I_SIZE, 0);
            wr32(dip + I_DTIME, fs_now(fs));
            ifree(fs, dino, 1);
        } else {
            uint16_t links = rd16(dip + I_LINKS);
            if (links > 0)
                wr16(dip + I_LINKS, (uint16_t)(--links));
            if (links == 0) {
                inode_free_blocks(fs, dip);
                wr32(dip + I_SIZE, 0);
                wr32(dip + I_DTIME, fs_now(fs));
                ifree(fs, dino, 0);
            }
        }
    }

    /* Link the source inode under the destination name, then unlink the
     * source name.  The source inode's link count is left untouched: a file
     * keeps its single link, a directory keeps the two it always has. */
    int r = dir_add(fs, dparent, dleaf, dleaf_len, sino, smode);
    if (r != EXT2_OK)
        return r;

    r = dir_remove(fs, sparent, sleaf, sleaf_len);
    if (r != EXT2_OK)
        return r;

    /* A moved directory's ".." now points at the destination parent. */
    if (sisdir) {
        uint8_t *sp = inode_ptr(fs, sparent);
        if (sp && rd16(sp + I_LINKS) > 1)
            wr16(sp + I_LINKS, (uint16_t)(rd16(sp + I_LINKS) - 1));
        uint8_t *dp = inode_ptr(fs, dparent);
        if (dp)
            wr16(dp + I_LINKS, (uint16_t)(rd16(dp + I_LINKS) + 1));
    }
    return EXT2_OK;
}
