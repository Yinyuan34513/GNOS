/*
 * ata.c — legacy ATA (IDE) disks over programmed I/O. (GPLv2)
 *
 * The protocol, in one paragraph.  Each channel owns eight command registers
 * at a fixed base (0x1F0 for the primary, 0x170 for the secondary) plus a
 * control register a little higher up (0x3F6 / 0x376).  You pick a drive by
 * writing the drive/head register, load a sector count and a sector address,
 * write a command byte, and then wait: the status register's BSY bit says the
 * drive is thinking and DRQ says it has a sector's worth of data waiting.
 * Each sector then moves through the 16-bit data register, 256 `in`/`out`
 * instructions at a time.  That is the whole interface, and it has been
 * compatible since 1986.
 *
 * Three details cause more bugs here than everything else combined:
 *
 *   - After selecting a drive you must let 400 ns pass before the status
 *     register means anything, and the canonical way to spend it is four
 *     reads of the *alternate* status port (which, unlike the real one, does
 *     not acknowledge interrupts).
 *
 *   - An absent drive does not report itself absent; the bus floats and reads
 *     back 0xFF.  Every wait loop is therefore bounded, because the failure
 *     mode of an unbounded one is a kernel that hangs at boot on hardware
 *     that merely does not exist.
 *
 *   - A CD-ROM answers on the same ports and fails IDENTIFY with a signature
 *     in the two LBA registers.  QEMU puts the boot ISO on the secondary
 *     master, so this is not a theoretical case: it is on every boot.
 */
#include <stdint.h>

#include "ata.h"
#include "io.h"
#include "vfs.h"
#include "subsys.h"
#include "kstring.h"
#include "debugcon.h"

/* ---- register map ------------------------------------------------------ */
/* Offsets from the command-block base. */
#define R_DATA      0
#define R_ERROR     1      /* read  */
#define R_FEATURES  1      /* write */
#define R_SECCOUNT  2
#define R_LBA0      3
#define R_LBA1      4
#define R_LBA2      5
#define R_DRIVE     6
#define R_STATUS    7      /* read  */
#define R_COMMAND   7      /* write */

/* Status bits. */
#define S_ERR   0x01
#define S_DRQ   0x08
#define S_DF    0x20
#define S_RDY   0x40
#define S_BSY   0x80

/* Commands. */
#define C_READ_PIO      0x20
#define C_READ_PIO_EXT  0x24
#define C_WRITE_PIO     0x30
#define C_WRITE_PIO_EXT 0x34
#define C_FLUSH         0xE7
#define C_FLUSH_EXT     0xEA
#define C_IDENTIFY      0xEC

/* Device control register (the control block's only byte we write). */
#define DCR_NIEN  0x02     /* stop the drive asserting its IRQ line */

/*
 * How many status reads to spend waiting.  Each `inb` on a legacy ISA port
 * costs on the order of a microsecond, so this is seconds of patience -- far
 * more than a spin-up needs, and still bounded.
 */
#define WAIT_SPINS  2000000

/* ---- devices ----------------------------------------------------------- */
#define MAX_DISKS   4      /* two channels, master and slave on each */
#define MAX_PARTS   4      /* an MBR has four primary entries and no more */

typedef struct {
    uint16_t io;           /* command block base    */
    uint16_t ctrl;         /* control block base    */
    uint8_t  slave;        /* 0 = master, 1 = slave */
    uint8_t  lba48;
    uint64_t sectors;      /* capacity, in 512-byte sectors */
    char     model[41];
} ata_disk_t;

/*
 * What a /dev node points at.  The whole disk and each of its partitions use
 * the same structure and the same operations; a partition is just a window
 * with a non-zero start and a shorter length.  Doing it this way means the
 * read/write path has no idea partitions exist, which is exactly the amount
 * it needs to know.
 */
typedef struct {
    ata_disk_t *disk;
    uint64_t    lba0;      /* first sector of the window   */
    uint64_t    nsect;     /* window length, in sectors    */
    char        name[8];   /* "sda", "sda1"                */
    uint8_t     is_part;
    uint8_t     used;
} ata_bdev_t;

static ata_disk_t g_disks[MAX_DISKS];
static int        g_ndisks;

/* Slot 0..MAX_DISKS-1 are the whole disks; the rest are partition windows,
 * MAX_PARTS per disk, indexed so a re-read can find and replace them. */
static ata_bdev_t g_bdevs[MAX_DISKS + MAX_DISKS * MAX_PARTS];

static ata_bdev_t *part_slot(int disk, int idx)
{
    return &g_bdevs[MAX_DISKS + disk * MAX_PARTS + idx];
}

/* ---- low-level waiting ------------------------------------------------- */
/*
 * The 400 ns settle after a drive select.  Reading the alternate status port
 * is the traditional way to burn it: it takes the same time as a real status
 * read but has no side effects on the interrupt line.
 */
static void ata_delay400(const ata_disk_t *d)
{
    for (int i = 0; i < 4; i++)
        (void)inb(d->ctrl);
}

/* Wait for BSY to clear.  Returns the final status, or -1 on timeout. */
static int ata_wait_ready(const ata_disk_t *d)
{
    for (uint32_t i = 0; i < WAIT_SPINS; i++) {
        uint8_t st = inb(d->io + R_STATUS);
        if (st == 0xFF)                /* floating bus: nothing is there */
            return -1;
        if (!(st & S_BSY))
            return st;
    }
    return -1;
}

/* Wait for BSY to clear *and* DRQ to appear, which is the drive saying "the
 * next 512 bytes are in the data register".  An ERR or DF in the meantime is
 * a real failure and must not be waited out. */
static int ata_wait_drq(const ata_disk_t *d)
{
    for (uint32_t i = 0; i < WAIT_SPINS; i++) {
        uint8_t st = inb(d->io + R_STATUS);
        if (st == 0xFF)
            return -1;
        if (st & (S_ERR | S_DF))
            return -1;
        if (!(st & S_BSY) && (st & S_DRQ))
            return 0;
    }
    return -1;
}

/* Select master or slave on this channel and let the selection settle. */
static void ata_select(const ata_disk_t *d, uint8_t head_bits)
{
    outb(d->io + R_DRIVE, (uint8_t)(0xE0 | (d->slave << 4) | head_bits));
    ata_delay400(d);
}

/* ---- IDENTIFY ---------------------------------------------------------- */
/*
 * IDENTIFY DEVICE returns 256 words describing the drive.  The three fields
 * that matter here:
 *
 *   words 27..46  model string, ASCII, byte-swapped within each word
 *   words 60..61  capacity in LBA28 sectors (0 if the drive is too big)
 *   word  83 b10  "supports 48-bit addressing"
 *   words 100..103 capacity in LBA48 sectors
 */
static int ata_identify(ata_disk_t *d)
{
    /* Interrupts off for this channel: the driver polls, and a drive that
     * asserts IRQ 14 with nobody listening leaves the PIC latched. */
    outb(d->ctrl, DCR_NIEN);

    ata_select(d, 0);
    outb(d->io + R_SECCOUNT, 0);
    outb(d->io + R_LBA0, 0);
    outb(d->io + R_LBA1, 0);
    outb(d->io + R_LBA2, 0);
    outb(d->io + R_COMMAND, C_IDENTIFY);
    ata_delay400(d);

    /* A status of zero means "no device here at all" -- distinct from the
     * floating 0xFF, and equally final. */
    uint8_t st = inb(d->io + R_STATUS);
    if (st == 0 || st == 0xFF)
        return 0;

    if (ata_wait_ready(d) < 0)
        return 0;

    /* ATAPI (a CD-ROM) refuses IDENTIFY and leaves its signature behind in
     * the LBA mid/high registers: 0x14/0xEB for ATAPI, 0x69/0x96 for SATAPI.
     * There is no point continuing; this driver speaks ATA, not the SCSI
     * command set tunnelled over ATAPI. */
    uint8_t lba1 = inb(d->io + R_LBA1);
    uint8_t lba2 = inb(d->io + R_LBA2);
    if ((lba1 == 0x14 && lba2 == 0xEB) || (lba1 == 0x69 && lba2 == 0x96))
        return 0;
    if (lba1 || lba2)                  /* some other non-ATA signature */
        return 0;

    if (ata_wait_drq(d) < 0)
        return 0;

    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(d->io + R_DATA);

    d->lba48 = (id[83] & (1u << 10)) != 0;

    uint64_t lba28 = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    uint64_t lba48 = (uint64_t)id[100]        | ((uint64_t)id[101] << 16) |
                     ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    d->sectors = (d->lba48 && lba48) ? lba48 : lba28;
    if (!d->sectors)
        return 0;

    /* The model string is stored big-endian within each word, so it reads as
     * "TQMEU DAHRDDI SK" until the halves are swapped back. */
    for (int i = 0; i < 20; i++) {
        d->model[i * 2]     = (char)(id[27 + i] >> 8);
        d->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    d->model[40] = 0;
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--)
        d->model[i] = 0;

    return 1;
}

/* ---- sector transfer --------------------------------------------------- */
/*
 * Address one sector and issue a command.  LBA48 is used whenever the drive
 * has it, not only when the address needs it: the two register layouts differ
 * only in that the 48-bit one writes the high bytes first, and picking a
 * single path removes a whole class of "works until the disk gets big" bug.
 */
static int ata_cmd_one(const ata_disk_t *d, uint64_t lba, uint8_t cmd28,
                       uint8_t cmd48)
{
    if (ata_wait_ready(d) < 0)
        return -1;

    if (d->lba48) {
        outb(d->io + R_DRIVE, (uint8_t)(0x40 | (d->slave << 4)));
        ata_delay400(d);
        outb(d->io + R_SECCOUNT, 0);                       /* count high */
        outb(d->io + R_LBA0, (uint8_t)(lba >> 24));
        outb(d->io + R_LBA1, (uint8_t)(lba >> 32));
        outb(d->io + R_LBA2, (uint8_t)(lba >> 40));
        outb(d->io + R_SECCOUNT, 1);                       /* count low  */
        outb(d->io + R_LBA0, (uint8_t)(lba));
        outb(d->io + R_LBA1, (uint8_t)(lba >> 8));
        outb(d->io + R_LBA2, (uint8_t)(lba >> 16));
        outb(d->io + R_COMMAND, cmd48);
    } else {
        /* In LBA28 the top four address bits live in the drive register,
         * which is also where the master/slave bit is. */
        outb(d->io + R_DRIVE,
             (uint8_t)(0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F)));
        ata_delay400(d);
        outb(d->io + R_FEATURES, 0);
        outb(d->io + R_SECCOUNT, 1);
        outb(d->io + R_LBA0, (uint8_t)(lba));
        outb(d->io + R_LBA1, (uint8_t)(lba >> 8));
        outb(d->io + R_LBA2, (uint8_t)(lba >> 16));
        outb(d->io + R_COMMAND, cmd28);
    }
    ata_delay400(d);
    return 0;
}

static int ata_read_sector(const ata_disk_t *d, uint64_t lba, uint8_t *buf)
{
    if (ata_cmd_one(d, lba, C_READ_PIO, C_READ_PIO_EXT) < 0)
        return -1;
    if (ata_wait_drq(d) < 0)
        return -1;

    uint16_t *w = (uint16_t *)buf;
    for (int i = 0; i < ATA_SECTOR / 2; i++)
        w[i] = inw(d->io + R_DATA);

    ata_delay400(d);
    return 0;
}

static int ata_write_sector(const ata_disk_t *d, uint64_t lba,
                            const uint8_t *buf)
{
    if (ata_cmd_one(d, lba, C_WRITE_PIO, C_WRITE_PIO_EXT) < 0)
        return -1;
    if (ata_wait_drq(d) < 0)
        return -1;

    /*
     * The data-out loop must not be tightened into a `rep outsw`.  The ATA
     * specification requires a gap between consecutive word writes -- real
     * drives latch on the trailing edge and can drop a word without one --
     * and a jmp-to-next-instruction is the traditional way to provide it.
     */
    const uint16_t *w = (const uint16_t *)buf;
    for (int i = 0; i < ATA_SECTOR / 2; i++) {
        outw(d->io + R_DATA, w[i]);
        io_delay();
    }

    /* The write is in the drive's cache until it is flushed; without this a
     * power cut (or a QEMU exit) between the write and the next boot loses
     * data the caller was told had landed. */
    outb(d->io + R_COMMAND, d->lba48 ? C_FLUSH_EXT : C_FLUSH);
    ata_delay400(d);
    if (ata_wait_ready(d) < 0)
        return -1;
    return 0;
}

/* ---- the /dev node ----------------------------------------------------- */
/*
 * Byte-addressed read and write on top of a sector-addressed device.  The
 * head and tail of a transfer are almost never sector-aligned, so both are
 * done through a bounce buffer: read the sector, patch the interesting part,
 * write it back.  That read-modify-write is what makes `echo x > /dev/sda`
 * change one byte instead of destroying 511 of its neighbours.
 */
static int32_t clamp(const ata_bdev_t *b, uint64_t off, uint32_t len,
                     uint64_t *end)
{
    uint64_t cap = b->nsect * ATA_SECTOR;
    if (off >= cap)
        return 0;                      /* past the end reads as EOF */
    if (off + len > cap)
        len = (uint32_t)(cap - off);
    *end = off + len;
    return (int32_t)len;
}

static int32_t bdev_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    ata_bdev_t *b = (ata_bdev_t *)n->priv;
    if (!b || !b->used)
        return -E_IO;

    uint64_t end;
    int32_t  want = clamp(b, off, len, &end);
    if (want <= 0)
        return want;

    uint8_t *out = (uint8_t *)buf;
    uint8_t  sec[ATA_SECTOR];
    uint64_t pos = off;

    while (pos < end) {
        uint64_t lba   = b->lba0 + pos / ATA_SECTOR;
        uint32_t skip  = (uint32_t)(pos % ATA_SECTOR);
        uint32_t chunk = ATA_SECTOR - skip;
        if (chunk > end - pos)
            chunk = (uint32_t)(end - pos);

        if (ata_read_sector(b->disk, lba, sec) < 0)
            return (pos > off) ? (int32_t)(pos - off) : -E_IO;
        memcpy(out, sec + skip, chunk);

        out += chunk;
        pos += chunk;
    }
    return want;
}

static int32_t bdev_write(vfs_node_t *n, uint64_t off, const void *buf,
                          uint32_t len)
{
    ata_bdev_t *b = (ata_bdev_t *)n->priv;
    if (!b || !b->used)
        return -E_IO;

    uint64_t end;
    int32_t  want = clamp(b, off, len, &end);
    if (want <= 0)
        return want == 0 ? -E_NOSPC : want;   /* a write past the end is ENOSPC */

    const uint8_t *in = (const uint8_t *)buf;
    uint8_t        sec[ATA_SECTOR];
    uint64_t       pos = off;

    while (pos < end) {
        uint64_t lba   = b->lba0 + pos / ATA_SECTOR;
        uint32_t skip  = (uint32_t)(pos % ATA_SECTOR);
        uint32_t chunk = ATA_SECTOR - skip;
        if (chunk > end - pos)
            chunk = (uint32_t)(end - pos);

        if (chunk == ATA_SECTOR) {
            memcpy(sec, in, ATA_SECTOR);       /* whole sector: no read needed */
        } else {
            if (ata_read_sector(b->disk, lba, sec) < 0)
                return (pos > off) ? (int32_t)(pos - off) : -E_IO;
            memcpy(sec + skip, in, chunk);
        }
        if (ata_write_sector(b->disk, lba, sec) < 0)
            return (pos > off) ? (int32_t)(pos - off) : -E_IO;

        in  += chunk;
        pos += chunk;
    }
    return want;
}

/* The block ioctls Linux defines; a partitioner or mkfs asks for these before
 * it will touch anything. */
#define BLKRRPART     0x125F
#define BLKGETSIZE    0x1260
#define BLKFLSBUF     0x1261
#define BLKSSZGET     0x1268
#define BLKGETSIZE64  0x80081272

static int ata_scan_partitions(int disk);

static int32_t bdev_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    ata_bdev_t *b = (ata_bdev_t *)n->priv;
    if (!b || !b->used)
        return -E_IO;

    switch (cmd) {
    case BLKGETSIZE64:
        if (!arg)
            return -E_FAULT;
        *(uint64_t *)(uintptr_t)arg = b->nsect * ATA_SECTOR;
        return 0;

    case BLKGETSIZE:                   /* the old one: a count of sectors */
        if (!arg)
            return -E_FAULT;
        *(unsigned long *)(uintptr_t)arg = (unsigned long)b->nsect;
        return 0;

    case BLKSSZGET:
        if (!arg)
            return -E_FAULT;
        *(int *)(uintptr_t)arg = ATA_SECTOR;
        return 0;

    case BLKFLSBUF:
        return 0;                      /* nothing is cached to flush */

    case BLKRRPART:
        /* Re-reading the table of a partition makes no sense; only the whole
         * disk has one.  This is what Linux answers too. */
        if (b->is_part)
            return -E_INVAL;
        return ata_scan_partitions((int)(b - g_bdevs));

    default:
        return -E_NOTTY;
    }
}

static const vfs_ops_t g_bdev_ops = {
    .read  = bdev_read,
    .write = bdev_write,
    .ioctl = bdev_ioctl,
    .mmap  = 0,
};

/* ---- the partition table ----------------------------------------------- */
/*
 * A classic MBR: 446 bytes of boot code, four 16-byte partition entries, and
 * the two-byte 0xAA55 signature.  Each entry is
 *
 *   0  status (0x80 = bootable)
 *   4  type   (0x00 = unused, 0x83 = Linux, 0xEE = protective GPT)
 *   8  first LBA, little-endian
 *  12  sector count, little-endian
 *
 * GPT is not handled.  A GPT disk presents a single type-0xEE partition
 * covering the whole device, which this code will publish as sda1 -- wrong,
 * but visibly wrong rather than silently ignored.
 */
#define MBR_SIG_OFF   510
#define MBR_PART_OFF  446

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void part_name(char *dst, const char *disk, int idx)
{
    int i = 0;
    while (disk[i] && i < 6) {
        dst[i] = disk[i];
        i++;
    }
    dst[i++] = (char)('1' + idx);
    dst[i]   = 0;
}

/*
 * (Re-)publish /dev/sdaN for one disk.  Every existing partition node for
 * that disk is withdrawn first: after an installer rewrites the table, a
 * leftover window pointing into what is now somebody else's partition is a
 * corruption waiting to happen.
 */
static int ata_scan_partitions(int disk)
{
    if (disk < 0 || disk >= g_ndisks)
        return -E_INVAL;

    ata_bdev_t *whole = &g_bdevs[disk];

    for (int i = 0; i < MAX_PARTS; i++) {
        ata_bdev_t *p = part_slot(disk, i);
        if (p->used) {
            vfs_unregister_dev(p->name);
            p->used = 0;
        }
    }

    uint8_t mbr[ATA_SECTOR];
    if (ata_read_sector(whole->disk, whole->lba0, mbr) < 0)
        return -E_IO;
    if (mbr[MBR_SIG_OFF] != 0x55 || mbr[MBR_SIG_OFF + 1] != 0xAA)
        return 0;                      /* unpartitioned; not an error */

    int found = 0;
    for (int i = 0; i < MAX_PARTS; i++) {
        const uint8_t *e     = mbr + MBR_PART_OFF + i * 16;
        uint8_t        type  = e[4];
        uint32_t       start = rd32(e + 8);
        uint32_t       count = rd32(e + 12);

        if (!type || !count)
            continue;
        /* An entry that runs off the end of the disk is a corrupt table, not
         * a device: publishing it would let a write wander past the last
         * sector and fail one I/O at a time. */
        if (start >= whole->nsect || (uint64_t)start + count > whole->nsect)
            continue;

        ata_bdev_t *p = part_slot(disk, i);
        p->disk    = whole->disk;
        p->lba0    = whole->lba0 + start;
        p->nsect   = count;
        p->is_part = 1;
        p->used    = 1;
        part_name(p->name, whole->name, i);

        if (vfs_register_blkdev(p->name, &g_bdev_ops, p,
                                p->nsect * ATA_SECTOR) != 0) {
            p->used = 0;
            continue;
        }
        found++;

        dbg_puts("ATA:   /dev/");
        dbg_puts(p->name);
        dbg_puts(" type=0x");
        dbg_puts_hexn(type, 2);
        dbg_puts(" start=");
        dbg_puts_dec(start);
        dbg_puts(" sectors=");
        dbg_puts_dec(count);
        dbg_puts("\n");
    }
    return found;
}

/* ---- probe ------------------------------------------------------------- */
static const struct { uint16_t io, ctrl; } g_channels[] = {
    { 0x1F0, 0x3F6 },
    { 0x170, 0x376 },
};

/* ---- disk-root boot ----------------------------------------------------- */
/* Read the whole *first used partition* of disk 0 into `dst`, which must be
 * `cap` bytes.  Returns the byte count, or 0 when there is no disk or no
 * usable partition.  This is the installer's boot path: when no initrd came
 * in RAM, the root filesystem is whatever the installer wrote to the disk,
 * and it has to be picked up whole here, before paging has anything
 * user-visible to attach it to.  Nothing is learned per-sector; a partition
 * is a window, and the read is just a window-long series of that one call. */
int ata_read_boot_partition(uint8_t *dst, uint32_t cap)
{
    if (!g_ndisks)
        return 0;

    ata_bdev_t *p = NULL;
    for (int i = 0; i < MAX_PARTS; i++) {
        if (!part_slot(0, i)->used)
            continue;
        p = part_slot(0, i);
        break;
    }
    if (!p)
        return 0;

    uint64_t total = p->nsect * ATA_SECTOR;
    if (total > cap)
        return 0;

    for (uint64_t i = 0; i < p->nsect; i++)
        if (ata_read_sector(p->disk, p->lba0 + i, dst + i * ATA_SECTOR) < 0)
            return 0;
    return (int)total;
}

int ata_init(void)
{
    /* Once is enough: the disks are there at boot or they never are, and a
     * second pass would re-identify live drives behind the nodes already
     * published -- and, in the disk-root build, smash the very sectors the
     * root filesystem was just copied out of. */
    static int g_ata_inited;
    if (g_ata_inited)
        return g_ndisks;
    g_ata_inited = 1;

    int slot = subsys_register("ata", "sda", SUBSYS_CLASS_BLOCK, 8, 0);

    for (unsigned c = 0; c < sizeof g_channels / sizeof g_channels[0]; c++) {
        for (int s = 0; s < 2; s++) {
            if (g_ndisks >= MAX_DISKS)
                break;

            ata_disk_t probe;
            memset(&probe, 0, sizeof probe);
            probe.io    = g_channels[c].io;
            probe.ctrl  = g_channels[c].ctrl;
            probe.slave = (uint8_t)s;

            if (!ata_identify(&probe))
                continue;

            int         idx = g_ndisks++;
            ata_disk_t *d   = &g_disks[idx];
            *d = probe;

            ata_bdev_t *b = &g_bdevs[idx];
            b->disk    = d;
            b->lba0    = 0;
            b->nsect   = d->sectors;
            b->is_part = 0;
            b->used    = 1;
            b->name[0] = 's'; b->name[1] = 'd';
            b->name[2] = (char)('a' + idx); b->name[3] = 0;

            if (vfs_register_blkdev(b->name, &g_bdev_ops, b,
                                    b->nsect * ATA_SECTOR) != 0) {
                b->used = 0;
                g_ndisks--;
                continue;
            }

            /* Each disk after the first gets its own registry entry, so
             * coldplug can assert on every node and not just /dev/sda. */
            if (idx > 0)
                subsys_set_state(subsys_register(b->name, b->name,
                                                 SUBSYS_CLASS_BLOCK, 8,
                                                 (uint16_t)(idx * 16)),
                                 SUBSYS_STATE_LIVE);

            dbg_puts("ATA: /dev/");
            dbg_puts(b->name);
            dbg_puts(" ");
            dbg_puts_dec((uint32_t)(d->sectors / 2048));
            dbg_puts(" MiB, lba");
            dbg_puts(d->lba48 ? "48" : "28");
            dbg_puts(", \"");
            dbg_puts(d->model);
            dbg_puts("\"\n");

            ata_scan_partitions(idx);
        }
    }

    if (!g_ndisks) {
        dbg_puts("ATA: no disks\n");
        subsys_set_state(slot, SUBSYS_STATE_FAILED);
        return 0;
    }
    subsys_set_state(slot, SUBSYS_STATE_LIVE);
    return g_ndisks;
}
