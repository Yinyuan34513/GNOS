/*
 * fat.c — read/write FAT12/16/32 driver over an in-memory image. (GPLv2)
 *
 * The FAT flavour is not stored anywhere in the BPB: per the Microsoft spec
 * it is *derived* from the number of data clusters (<4085 => FAT12,
 * <65525 => FAT16, otherwise FAT32).  Everything else in here follows from
 * that: where the root directory lives, how wide a FAT entry is, and which
 * value marks the end of a cluster chain.
 *
 * The write side is the same code read backwards.  Three invariants are worth
 * stating, because every mutating function below depends on them:
 *
 *   - A file *is* its cluster chain plus one 32-byte directory entry.  Change
 *     one without the other and the volume is corrupt, so every routine that
 *     touches a chain calls ent_sync() before it returns.
 *
 *   - There are num_fats copies of the allocation table and they must agree.
 *     fat_set_next() therefore always writes all of them; nothing else in the
 *     driver is allowed to poke at a FAT.
 *
 *   - A newly allocated cluster is zeroed.  Directories rely on this: an
 *     all-zero entry is the "no more entries" marker, so a fresh directory
 *     cluster is an empty directory for free.
 */
#include <stddef.h>
#include <stdint.h>

#include "fat.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint8_t *sector(fat_fs_t *fs, uint32_t s)
{
    uint64_t off = (uint64_t)s * fs->bytes_per_sec;
    if (off + fs->bytes_per_sec > fs->img_size)
        return NULL;
    return fs->img + off;
}

static uint32_t clus_to_sec(const fat_fs_t *fs, uint32_t clus)
{
    return fs->data_start + (clus - 2) * fs->sec_per_clus;
}

static int clus_ok(const fat_fs_t *fs, uint32_t clus)
{
    return clus >= 2 && clus < fs->clus_count + 2;
}

/* A whole cluster's worth of image, bounds-checked. */
static uint8_t *clus_ptr(fat_fs_t *fs, uint32_t clus)
{
    if (!clus_ok(fs, clus))
        return NULL;

    uint64_t off = (uint64_t)clus_to_sec(fs, clus) * fs->bytes_per_sec;
    uint64_t len = (uint64_t)fs->bytes_per_sec * fs->sec_per_clus;
    if (off + len > fs->img_size)
        return NULL;
    return fs->img + off;
}

/* End-of-chain marker for the detected FAT width. */
static int fat_is_eoc(const fat_fs_t *fs, uint32_t v)
{
    switch (fs->type) {
    case 12: return v >= 0x0FF8;
    case 16: return v >= 0xFFF8;
    default: return v >= 0x0FFFFFF8;
    }
}

static uint32_t fat_next(fat_fs_t *fs, uint32_t clus)
{
    if (!clus_ok(fs, clus))
        return 0x0FFFFFFF;                 /* out of range: treat as EOC */

    const uint8_t *fat = fs->img + (uint64_t)fs->fat_start * fs->bytes_per_sec;

    switch (fs->type) {
    case 12: {
        uint32_t off = clus + (clus / 2);          /* 1.5 bytes per entry */
        uint16_t v   = rd16(fat + off);
        return (clus & 1) ? (uint32_t)(v >> 4) : (uint32_t)(v & 0x0FFF);
    }
    case 16:
        return rd16(fat + clus * 2);
    default:
        return rd32(fat + clus * 4) & 0x0FFFFFFF;
    }
}

/* Store a FAT entry in *every* copy of the table: they are redundant on
 * purpose and a mismatch is what fsck calls a corrupt volume. */
static void fat_set_next(fat_fs_t *fs, uint32_t clus, uint32_t val)
{
    if (!clus_ok(fs, clus))
        return;

    for (uint32_t f = 0; f < fs->num_fats; f++) {
        uint64_t base = ((uint64_t)fs->fat_start +
                         (uint64_t)f * fs->sec_per_fat) * fs->bytes_per_sec;
        if (base + fs->bytes_per_sec > fs->img_size)
            return;
        uint8_t *fat = fs->img + base;

        switch (fs->type) {
        case 12: {
            uint32_t off = clus + (clus / 2);
            uint16_t v   = rd16(fat + off);
            if (clus & 1)
                v = (uint16_t)((v & 0x000F) | ((val & 0x0FFF) << 4));
            else
                v = (uint16_t)((v & 0xF000) | (val & 0x0FFF));
            wr16(fat + off, v);
            break;
        }
        case 16:
            wr16(fat + clus * 2, (uint16_t)val);
            break;
        default: {
            /* The top four bits of a FAT32 entry are reserved. */
            uint32_t old = rd32(fat + clus * 4);
            wr32(fat + clus * 4, (old & 0xF0000000u) | (val & 0x0FFFFFFFu));
            break;
        }
        }
    }
}

static void zero_clus(fat_fs_t *fs, uint32_t clus)
{
    uint8_t *p = clus_ptr(fs, clus);
    if (!p)
        return;

    uint32_t n = (uint32_t)fs->bytes_per_sec * fs->sec_per_clus;
    for (uint32_t i = 0; i < n; i++)
        p[i] = 0;
}

/* First-fit from wherever the last allocation left off.  Returns 0 when the
 * volume is full. */
static uint32_t fat_alloc_clus(fat_fs_t *fs)
{
    uint32_t total = fs->clus_count + 2;
    uint32_t c     = fs->alloc_hint;

    if (c < 2 || c >= total)
        c = 2;

    for (uint32_t n = 0; n < fs->clus_count; n++) {
        if (fat_next(fs, c) == 0) {
            fat_set_next(fs, c, 0x0FFFFFFF);       /* claim it as a chain end */
            zero_clus(fs, c);
            fs->alloc_hint = (c + 1 < total) ? c + 1 : 2;
            return c;
        }
        if (++c >= total)
            c = 2;
    }
    return 0;
}

static void fat_free_chain(fat_fs_t *fs, uint32_t clus)
{
    while (clus_ok(fs, clus)) {
        uint32_t next = fat_next(fs, clus);
        fat_set_next(fs, clus, 0);
        if (fat_is_eoc(fs, next))
            break;
        clus = next;
    }
}

/* Next cluster of a chain; with `grow` set, append a fresh one if the chain
 * ends here.  Returns 0 when there is nowhere to go. */
static uint32_t chain_step(fat_fs_t *fs, uint32_t clus, int grow)
{
    uint32_t n = fat_next(fs, clus);
    if (clus_ok(fs, n) && !fat_is_eoc(fs, n))
        return n;
    if (!grow)
        return 0;

    n = fat_alloc_clus(fs);
    if (!n)
        return 0;
    fat_set_next(fs, clus, n);
    return n;
}

int fat_mount(fat_fs_t *fs, uint8_t *img, uint32_t img_size)
{
    if (!img || img_size < 512)
        return 0;

    fs->img           = img;
    fs->img_size      = img_size;
    fs->bytes_per_sec = rd16(img + 11);
    fs->sec_per_clus  = img[13];
    fs->reserved_secs = rd16(img + 14);
    fs->num_fats      = img[16];
    fs->root_entries  = rd16(img + 17);
    fs->alloc_hint    = 2;

    if (fs->bytes_per_sec == 0 || fs->sec_per_clus == 0 || fs->num_fats == 0)
        return 0;

    uint32_t fatsz16 = rd16(img + 22);
    fs->sec_per_fat  = fatsz16 ? fatsz16 : rd32(img + 36);   /* BPB_FATSz32 */

    uint32_t tot16   = rd16(img + 19);
    fs->total_secs   = tot16 ? tot16 : rd32(img + 32);       /* BPB_TotSec32 */

    if (fs->sec_per_fat == 0 || fs->total_secs == 0)
        return 0;

    fs->root_secs  = ((uint32_t)fs->root_entries * 32 + fs->bytes_per_sec - 1)
                     / fs->bytes_per_sec;
    fs->fat_start  = fs->reserved_secs;
    fs->root_start = fs->fat_start + (uint32_t)fs->num_fats * fs->sec_per_fat;
    fs->data_start = fs->root_start + fs->root_secs;

    if (fs->total_secs <= fs->data_start)
        return 0;
    fs->clus_count = (fs->total_secs - fs->data_start) / fs->sec_per_clus;

    /* The one and only way to tell the three flavours apart. */
    if (fs->clus_count < 4085)
        fs->type = 12;
    else if (fs->clus_count < 65525)
        fs->type = 16;
    else
        fs->type = 32;

    fs->root_clus = (fs->type == 32) ? rd32(img + 44) : 0;   /* BPB_RootClus */
    return 1;
}

void fat_opendir(fat_fs_t *fs, uint32_t clus, fat_dir_t *dir)
{
    dir->fs    = fs;
    dir->index = 0;

    if (clus == 0 && fs->type != 32) {
        dir->fixed_root = 1;
        dir->clus       = 0;
    } else {
        dir->fixed_root = 0;
        dir->clus       = clus ? clus : fs->root_clus;
    }
}

/* Return a pointer to the raw 32-byte entry at dir->index, advancing through
 * the cluster chain as needed.  NULL once the directory is exhausted. */
static uint8_t *dir_entry_at(fat_dir_t *dir)
{
    fat_fs_t *fs     = dir->fs;
    uint32_t per_sec = fs->bytes_per_sec / 32;

    if (dir->fixed_root) {
        if (dir->index >= fs->root_entries)
            return NULL;
        uint32_t sec = fs->root_start + dir->index / per_sec;
        uint8_t *s = sector(fs, sec);
        return s ? s + (dir->index % per_sec) * 32 : NULL;
    }

    uint32_t per_clus = per_sec * fs->sec_per_clus;
    uint32_t clus     = dir->clus;
    uint32_t idx      = dir->index;

    /* Walk the chain to the cluster holding `idx`. */
    while (idx >= per_clus) {
        clus = fat_next(fs, clus);
        if (!clus_ok(fs, clus) || fat_is_eoc(fs, clus))
            return NULL;
        idx -= per_clus;
    }
    if (!clus_ok(fs, clus))
        return NULL;

    uint32_t sec = clus_to_sec(fs, clus) + idx / per_sec;
    uint8_t *s = sector(fs, sec);
    return s ? s + (idx % per_sec) * 32 : NULL;
}

/* "INIT    ELF" -> "INIT.ELF" */
static void name83_to_str(const uint8_t *de, char *out)
{
    int n = 0;
    for (int i = 0; i < 8 && de[i] != ' '; i++)
        out[n++] = (char)de[i];
    if (de[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && de[i] != ' '; i++)
            out[n++] = (char)de[i];
    }
    out[n] = 0;
}

int fat_readdir(fat_dir_t *dir, fat_dirent_t *out)
{
    for (;;) {
        uint8_t *de = dir_entry_at(dir);
        if (!de)
            return 0;
        dir->index++;

        if (de[0] == 0x00)
            return 0;                       /* no more entries at all      */
        if (de[0] == 0xE5)
            continue;                       /* deleted                     */
        if ((de[11] & FAT_ATTR_LFN) == FAT_ATTR_LFN)
            continue;                       /* long-name fragment          */
        if (de[11] & FAT_ATTR_VOLUME_ID)
            continue;                       /* volume label                */

        name83_to_str(de, out->name);
        out->attr       = de[11];
        out->size       = rd32(de + 28);
        out->first_clus = ((uint32_t)rd16(de + 20) << 16) | rd16(de + 26);
        if (dir->fs->type != 32)
            out->first_clus &= 0xFFFF;      /* the high word is garbage    */
        out->ent_off    = (uint32_t)(de - dir->fs->img);
        return 1;
    }
}

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* Case-insensitive compare of `a` against a path component [b, b_end). */
static int name_eq(const char *a, const char *b, const char *b_end)
{
    while (*a && b < b_end) {
        if (upper(*a) != upper(*b))
            return 0;
        a++; b++;
    }
    return *a == 0 && b == b_end;
}

int fat_lookup(fat_fs_t *fs, const char *path, fat_dirent_t *out)
{
    uint32_t clus = 0;                       /* 0 == root */

    while (*path == '/')
        path++;

    /* The root directory itself. */
    if (*path == 0) {
        out->name[0]    = '/';
        out->name[1]    = 0;
        out->attr       = FAT_ATTR_DIRECTORY;
        out->first_clus = (fs->type == 32) ? fs->root_clus : 0;
        out->size       = 0;
        out->ent_off    = 0;                 /* synthesised: not on disk */
        return 1;
    }

    for (;;) {
        const char *end = path;
        while (*end && *end != '/')
            end++;

        fat_dir_t dir;
        fat_opendir(fs, clus, &dir);

        int found = 0;
        while (fat_readdir(&dir, out)) {
            if (name_eq(out->name, path, end)) {
                found = 1;
                break;
            }
        }
        if (!found)
            return 0;

        while (*end == '/')
            end++;
        if (*end == 0)
            return 1;                        /* last component: done */

        if (!(out->attr & FAT_ATTR_DIRECTORY))
            return 0;                        /* "a/b" where a is a file */
        clus = out->first_clus;
        path = end;
    }
}

uint32_t fat_read(fat_fs_t *fs, const fat_dirent_t *ent,
                  uint32_t off, void *buf, uint32_t len)
{
    if (off >= ent->size)
        return 0;
    if (len > ent->size - off)
        len = ent->size - off;

    uint32_t clus_bytes = (uint32_t)fs->bytes_per_sec * fs->sec_per_clus;
    uint32_t clus       = ent->first_clus;
    uint8_t *dst        = (uint8_t *)buf;
    uint32_t done       = 0;

    /* Skip whole clusters until we reach the one containing `off`. */
    while (off >= clus_bytes) {
        clus = fat_next(fs, clus);
        if (!clus_ok(fs, clus) || fat_is_eoc(fs, clus))
            return 0;
        off -= clus_bytes;
    }

    while (done < len) {
        const uint8_t *src = clus_ptr(fs, clus);
        if (!src)
            break;

        uint32_t n = clus_bytes - off;
        if (n > len - done)
            n = len - done;
        for (uint32_t i = 0; i < n; i++)
            dst[done + i] = src[off + i];

        done += n;
        off   = 0;
        if (done >= len)
            break;

        clus = fat_next(fs, clus);
        if (!clus_ok(fs, clus) || fat_is_eoc(fs, clus))
            break;
    }

    return done;
}

/* ---- the mutating half ------------------------------------------------ */

/* Push a dirent's size and starting cluster back into the on-disk entry. */
static void ent_sync(fat_fs_t *fs, const fat_dirent_t *e)
{
    if (!e->ent_off || (uint64_t)e->ent_off + 32 > fs->img_size)
        return;

    uint8_t *de = fs->img + e->ent_off;
    wr16(de + 20, (uint16_t)(e->first_clus >> 16));
    wr16(de + 26, (uint16_t)(e->first_clus & 0xFFFF));
    /* A directory's size field is defined to be zero. */
    wr32(de + 28, (e->attr & FAT_ATTR_DIRECTORY) ? 0 : e->size);
}

uint32_t fat_write(fat_fs_t *fs, fat_dirent_t *ent,
                   uint32_t off, const void *buf, uint32_t len)
{
    const uint8_t *src  = (const uint8_t *)buf;
    uint32_t clus_bytes = (uint32_t)fs->bytes_per_sec * fs->sec_per_clus;
    uint32_t start      = off;
    uint32_t done       = 0;

    if (!len || (ent->attr & FAT_ATTR_DIRECTORY))
        return 0;

    /* An empty file has no chain at all; give it one. */
    if (!clus_ok(fs, ent->first_clus)) {
        uint32_t c = fat_alloc_clus(fs);
        if (!c)
            return 0;
        ent->first_clus = c;
        ent_sync(fs, ent);
    }

    uint32_t clus = ent->first_clus;

    /* Walk (and if necessary grow) the chain up to the starting offset: this
     * is what makes lseek-past-the-end-then-write allocate the gap. */
    while (off >= clus_bytes) {
        clus = chain_step(fs, clus, 1);
        if (!clus)
            break;
        off -= clus_bytes;
    }

    while (clus && done < len) {
        uint8_t *dst = clus_ptr(fs, clus);
        if (!dst)
            break;

        uint32_t n = clus_bytes - off;
        if (n > len - done)
            n = len - done;
        for (uint32_t i = 0; i < n; i++)
            dst[off + i] = src[done + i];

        done += n;
        off   = 0;
        if (done >= len)
            break;

        clus = chain_step(fs, clus, 1);
    }

    if (done && start + done > ent->size) {
        ent->size = start + done;
        ent_sync(fs, ent);
    }
    return done;
}

int fat_truncate(fat_fs_t *fs, fat_dirent_t *ent)
{
    if (!ent->ent_off)
        return FAT_EINVAL;
    if (ent->attr & FAT_ATTR_DIRECTORY)
        return FAT_EINVAL;

    if (clus_ok(fs, ent->first_clus))
        fat_free_chain(fs, ent->first_clus);
    ent->first_clus = 0;
    ent->size       = 0;
    ent_sync(fs, ent);
    return FAT_OK;
}

/* "/a/b/c.txt" -> dirbuf "/a/b", *leaf -> "c.txt".  A path with no directory
 * part yields "/". */
static int split_path(const char *path, char *dirbuf, int cap, const char **leaf)
{
    if (!path || path[0] != '/')
        return 0;

    const char *slash = path;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            slash = p;

    int n = (int)(slash - path);
    if (n <= 0) {
        dirbuf[0] = '/';
        dirbuf[1] = 0;
    } else {
        if (n >= cap)
            return 0;
        for (int i = 0; i < n; i++)
            dirbuf[i] = path[i];
        dirbuf[n] = 0;
    }

    *leaf = slash + 1;
    return **leaf != 0;
}

/* "readme.txt" -> "README  TXT".  Returns 0 for anything that will not fit in
 * an 8.3 name, which is the whole of our namespace. */
static int name_to_83(const char *name, uint8_t out[11])
{
    for (int i = 0; i < 11; i++)
        out[i] = ' ';

    int i = 0;
    while (*name && *name != '.') {
        if (i >= 8 || *name == '/')
            return 0;
        out[i++] = (uint8_t)upper(*name++);
    }
    if (i == 0)
        return 0;

    if (*name == '.') {
        name++;
        int j = 8;
        while (*name) {
            if (j >= 11 || *name == '.' || *name == '/')
                return 0;
            out[j++] = (uint8_t)upper(*name++);
        }
    }
    return 1;
}

static void put_entry(uint8_t *de, const uint8_t n83[11], uint8_t attr,
                      uint32_t clus, uint32_t size)
{
    for (int i = 0; i < 32; i++)
        de[i] = 0;
    for (int i = 0; i < 11; i++)
        de[i] = n83[i];

    de[11] = attr;
    wr16(de + 20, (uint16_t)(clus >> 16));
    wr16(de + 26, (uint16_t)(clus & 0xFFFF));
    wr32(de + 28, size);
}

/* Find a reusable 32-byte slot in a directory, growing it by one cluster if
 * every slot is taken.  `clus` == 0 means the root. */
static uint8_t *dir_alloc_slot(fat_fs_t *fs, uint32_t clus)
{
    fat_dir_t d;
    fat_opendir(fs, clus, &d);

    for (;;) {
        uint8_t *de = dir_entry_at(&d);
        if (!de)
            break;
        if (de[0] == 0x00 || de[0] == 0xE5)
            return de;
        d.index++;
    }

    if (d.fixed_root)
        return NULL;                     /* the FAT12/16 root cannot grow */

    uint32_t last = d.clus;
    if (!clus_ok(fs, last))
        return NULL;
    for (;;) {
        uint32_t n = fat_next(fs, last);
        if (!clus_ok(fs, n) || fat_is_eoc(fs, n))
            break;
        last = n;
    }

    uint32_t fresh = fat_alloc_clus(fs);
    if (!fresh)
        return NULL;
    fat_set_next(fs, last, fresh);
    return clus_ptr(fs, fresh);          /* zeroed, so slot 0 is free */
}

int fat_create(fat_fs_t *fs, const char *path, uint8_t attr, fat_dirent_t *out)
{
    fat_dirent_t existing;
    if (fat_lookup(fs, path, &existing))
        return FAT_EEXIST;

    char        dirpath[128];
    const char *leaf;
    if (!split_path(path, dirpath, (int)sizeof(dirpath), &leaf))
        return FAT_EINVAL;

    uint8_t n83[11];
    if (!name_to_83(leaf, n83))
        return FAT_EINVAL;

    /* Locate the parent.  Cluster 0 means "root", which is also what FAT
     * wants stored in a ".." entry that points back at the root. */
    uint32_t parent = 0;
    if (dirpath[1]) {
        fat_dirent_t pd;
        if (!fat_lookup(fs, dirpath, &pd))
            return FAT_ENOENT;
        if (!(pd.attr & FAT_ATTR_DIRECTORY))
            return FAT_ENOTDIR;
        parent = pd.first_clus;
    }

    uint32_t first = 0;
    if (attr & FAT_ATTR_DIRECTORY) {
        first = fat_alloc_clus(fs);
        if (!first)
            return FAT_ENOSPC;
    }

    uint8_t *de = dir_alloc_slot(fs, parent);
    if (!de) {
        if (first)
            fat_free_chain(fs, first);
        return FAT_ENOSPC;
    }

    put_entry(de, n83, attr, first, 0);

    if (attr & FAT_ATTR_DIRECTORY) {
        /* "." and ".." are ordinary entries; a directory without them is not
         * a directory as far as anything else is concerned. */
        uint8_t *body = clus_ptr(fs, first);
        if (body) {
            static const uint8_t dot[11]    = { '.', ' ', ' ', ' ', ' ', ' ',
                                                ' ', ' ', ' ', ' ', ' ' };
            static const uint8_t dotdot[11] = { '.', '.', ' ', ' ', ' ', ' ',
                                                ' ', ' ', ' ', ' ', ' ' };
            put_entry(body,      dot,    FAT_ATTR_DIRECTORY, first,  0);
            put_entry(body + 32, dotdot, FAT_ATTR_DIRECTORY, parent, 0);
        }
    }

    if (out) {
        name83_to_str(de, out->name);
        out->attr       = attr;
        out->first_clus = first;
        out->size       = 0;
        out->ent_off    = (uint32_t)(de - fs->img);
    }
    return FAT_OK;
}

int fat_unlink(fat_fs_t *fs, const char *path)
{
    fat_dirent_t e;
    if (!fat_lookup(fs, path, &e))
        return FAT_ENOENT;
    if (!e.ent_off)
        return FAT_EINVAL;                   /* the root directory */

    if (e.attr & FAT_ATTR_DIRECTORY) {
        fat_dir_t    d;
        fat_dirent_t child;
        fat_opendir(fs, e.first_clus, &d);
        while (fat_readdir(&d, &child)) {
            if (child.name[0] == '.' &&
                (child.name[1] == 0 ||
                 (child.name[1] == '.' && child.name[2] == 0)))
                continue;
            return FAT_ENOTEMPTY;
        }
    }

    if (clus_ok(fs, e.first_clus))
        fat_free_chain(fs, e.first_clus);

    fs->img[e.ent_off] = 0xE5;               /* the classic tombstone */
    return FAT_OK;
}
