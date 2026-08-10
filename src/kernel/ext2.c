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

/* Walk an absolute path down to its final component. */
static uint32_t path_walk(ext2_fs_t *fs, const char *path, uint32_t *parent_out,
                          const char **leaf_out, uint32_t *leaf_len_out)
{
    if (!path || path[0] != '/')
        return 0;

    uint32_t dir  = EXT2_ROOT_INO;
    uint32_t cur  = EXT2_ROOT_INO;
    const char *p = path;
    const char *leaf     = NULL;
    uint32_t    leaf_len = 0;

    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;

        uint32_t len = comp_len(p);
        dir  = cur;
        leaf = p;
        leaf_len = len;

        cur = dir_find(fs, dir, p, len);
        p += len;

        if (!cur) {
            /* Only the very last component may be missing; a missing
             * intermediate directory is a plain failure. */
            while (*p == '/')
                p++;
            if (*p)
                return 0;
            break;
        }
    }

    if (parent_out)
        *parent_out = dir;
    if (leaf_out)
        *leaf_out = leaf;
    if (leaf_len_out)
        *leaf_len_out = leaf_len;
    return cur;
}

int ext2_lookup(ext2_fs_t *fs, const char *path, ext2_dirent_t *out)
{
    if (!fs || !path || path[0] != '/')
        return 0;

    const char *leaf     = NULL;
    uint32_t    leaf_len = 0;
    uint32_t    ino      = path_walk(fs, path, NULL, &leaf, &leaf_len);

    if (!ino)
        return 0;

    if (out) {
        if (leaf && leaf_len)
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
                      const char *name, uint32_t len, int isdir)
{
    wr32(de + DE_INODE, ino);
    de[DE_NAME_LEN]  = (uint8_t)len;
    de[DE_FILE_TYPE] = fs->has_filetype
                     ? (uint8_t)(isdir ? EXT2_FT_DIR : EXT2_FT_REG) : 0;
    memcpy(de + DE_NAME, name, len);
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
                   uint32_t len, uint32_t ino, int isdir)
{
    if (!len || len > 255)
        return EXT2_EINVAL;

    uint8_t *dp = inode_ptr(fs, dino);
    if (!dp)
        return EXT2_ENOENT;
    if ((rd16(dp + I_MODE) & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return EXT2_ENOTDIR;

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
            put_entry(fs, slot, ino, name, len, isdir);
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
    put_entry(fs, b, ino, name, len, isdir);
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
    const char *leaf     = NULL;
    uint32_t    leaf_len = 0;

    if (path_walk(fs, path, &parent, &leaf, &leaf_len))
        return EXT2_EEXIST;
    if (!parent)
        return EXT2_ENOENT;              /* a directory along the way is missing */
    if (!leaf || !leaf_len || leaf_len > 255)
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

    int r = dir_add(fs, parent, leaf, leaf_len, ino, isdir);
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
    const char *leaf     = NULL;
    uint32_t    leaf_len = 0;
    uint32_t    ino      = path_walk(fs, path, &parent, &leaf, &leaf_len);

    if (!ino)
        return EXT2_ENOENT;
    if (!leaf || !leaf_len || ino == EXT2_ROOT_INO)
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
