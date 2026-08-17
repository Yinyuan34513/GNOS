/*
 * installer.c — the GNOS disk installer. (GPLv2)
 *
 * A menu on the first terminal that takes the running system, copied out of
 * its own RAM root filesystem, and dumps it onto a hard disk so the machine
 * can boot without a CD.  The three ingredients of a bootable disk are all
 * ordinary files here, because the live system *was* those files a moment
 * ago:
 *
 *   /proc/boot/root.img      the root filesystem, as one image
 *   /boot/limine/limine.sys  Limine's BIOS stage 1 + stage 2
 *   /boot/limine/limine.conf      the boot config, baked into the rootfs
 *
 * The orders of operation on the target disk:
 *
 *   1. stage 1 (the MBR half of limine.sys) goes to sector 0, with a fresh
 *      partition table welded in behind it -- the first entry owns the whole
 *      disk from LBA 2048 on;
 *   2. stage 2 (the rest of limine.sys) goes into the gap between the MBR
 *      and that partition;
 *   3. the kernel is told to re-read the table (BLKRRPART), which turns
 *      /dev/sda1 into a live node;
 *   4. the root image is copied to /dev/sda1, with a read-back of every
 *      sector after the copy -- a verify that costs a second pass over the
 *      disk and catches every kind of short write PIO is fond of;
 *   5. reboot(RB_POWER_OFF) ... and when the machine boots again it is the
 *      disk root the kernel reads, initrd-less, up the path the kernel's
 *      own fallback implements.
 *
 * Limine is the BIOS bootloader: it hands off to /GNOSKr.elf, which has
 * ridden inside the rootfs (see Makefile "boot payload") since the image
 * was built, and the kernel_image entry in the config names it.  There is
 * deliberately no UEFI story here yet.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#define BLKGETSIZE64 0x80081272       /* fstat helper: _IOR('B', 114, u64) */
#define BLKRRPART    0x125F           /* re-read the partition table */

#define ATA_SECTOR      512
#define PART_START_SECT 2048          /* 1 MiB in; the limine gap stays free */
#define BUF_SECTS       1024          /* copy/verify buffer: 512 KiB */
#define MAX_DISKS       6

/* ---- screen ------------------------------------------------------------ */
/* The installer owns the terminal for the duration: raw keys, no echo, a
 * small text UI drawn with ANSI escapes on 80 columns. */
static void term_raw(int fd, struct termios *saved)
{
    struct termios t;
    tcgetattr(fd, saved);
    t = *saved;
    cfmakeraw(&t);
    tcsetattr(fd, TCSANOW, &t);
}

static void term_restore(int fd, const struct termios *saved)
{
    tcsetattr(fd, TCSANOW, saved);
}

static void cls(void)
{
    fputs("\033[2J\033[H", stdout);
    fflush(stdout);
}

static void mv(int y, int x)
{
    printf("\033[%d;%dH", y, x + 1);
}

static void title(const char *s)
{
    mv(1, 0);
    printf("\033[7m  %-76s\033[0m\n", s);
}

static void hline(int y)
{
    mv(y, 0);
    fputs("\033[7m", stdout);
    for (int i = 0; i < 80; i++)
        fputc('-', stdout);
    fputs("\033[0m", stdout);
}

static void text(int y, int x, const char *s)
{
    mv(y, x);
    fputs(s, stdout);
    fflush(stdout);
}

static void menu_item(int y, int x, const char *label, int sel)
{
    if (sel) {
        mv(y, x);
        fputs("\033[7m", stdout);
        fputs(label, stdout);
        fputs("\033[0m", stdout);
    } else {
        mv(y, x);
        fputs(label, stdout);
    }
    fflush(stdout);
}

/* ---- disks ------------------------------------------------------------- */
struct disk {
    char     name[8];                  /* "sda" etc. */
    uint64_t bytes;
    int      fd;
    int      present;
};

static void find_disks(struct disk *d, int *n)
{
    *n = 0;
    for (int i = 0; i < MAX_DISKS; i++) {
        struct disk *di = &d[*n];
        snprintf(di->name, sizeof di->name, "/dev/sd%c", 'a' + i);
        di->fd = open(di->name, O_RDWR);
        if (di->fd < 0)
            return;                    /* sda exists => sdb might not: stop */
        if (ioctl(di->fd, BLKGETSIZE64, &di->bytes) < 0 || !di->bytes) {
            close(di->fd);
            continue;
        }
        di->present = 1;
        (*n)++;
    }
}

/* ---- the install itself ------------------------------------------------ */

static off_t file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return -1;
    return st.st_size;
}

/* Sector 0 of the target: limine's stage-1 boot code, our partition table,
 * and the 0xAA55 signature.  The partition entry (start LBA, count) arrives
 * per call. */
static int write_mbr(int fd, const uint8_t *stage1,
                     uint32_t part_start, uint32_t part_count)
{
    uint8_t mbr[ATA_SECTOR];
    memset(mbr, 0, sizeof mbr);
    memcpy(mbr, stage1, 440);          /* boot code only; no table inside */

    uint8_t *e = mbr + 446;            /* entry 0, the classic layout */
    e[0] = 0x80;                       /* bootable      */
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;      /* CHS start: wildcard */
    e[4] = 0x83;                       /* Linux         */
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;      /* CHS end: wildcard */
    e[8]  = part_start & 0xFF;         /* LBA start, LE */
    e[9]  = (part_start >> 8) & 0xFF;
    e[10] = (part_start >> 16) & 0xFF;
    e[11] = (part_start >> 24) & 0xFF;
    e[12] = part_count & 0xFF;         /* sector count, LE */
    e[13] = (part_count >> 8) & 0xFF;
    e[14] = (part_count >> 16) & 0xFF;
    e[15] = (part_count >> 24) & 0xFF;
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    if (pwrite(fd, mbr, ATA_SECTOR, 0) != ATA_SECTOR)
        return -1;
    return 0;
}

/* The copy of the root image onto the partitioned disk, with the read-back
 * verify folded into the same loop: after every 512 KiB write the matching
 * sectors are read again and compared.  A progress line is repainted at the
 * bottom of the screen. */
static int copy_root(int dst, int imgfd, uint64_t total, int y)
{
    static uint8_t buf[BUF_SECTS * ATA_SECTOR];
    static uint8_t cmp[BUF_SECTS * ATA_SECTOR];

    uint64_t done = 0;
    while (done < total) {
        uint64_t chunk = total - done;
        if (chunk > sizeof buf)
            chunk = sizeof buf;

        ssize_t r = read(imgfd, buf, (size_t)chunk);
        if (r <= 0)
            return r < 0 ? -1 : -2;    /* short image: -2 means "truncated" */
        if (pwrite(dst, buf, (size_t)r, (off_t)done) != r)
            return -1;
        if (pread(dst, cmp, (size_t)r, (off_t)done) != r)
            return -1;
        if (memcmp(buf, cmp, (size_t)r) != 0)
            return -3;                 /* read-back mismatch */

        done += (uint64_t)r;
        mv(y, 0);
        printf("\033[Kcopy: %llu / %llu KiB (%llu%%)\r",
               (unsigned long long)(done / 1024),
               (unsigned long long)(total / 1024),
               (unsigned long long)(done * 100 / total));
        fflush(stdout);
    }
    mv(y, 0);
    fputs("\033[K", stdout);
    fflush(stdout);
    return 0;
}

static uint32_t image_sectors(void)
{
    off_t sz = file_size("/proc/boot/root.img");
    if (sz <= 0)
        return 0;
    return (uint32_t)((sz + ATA_SECTOR - 1) / ATA_SECTOR);
}

/* ---- the screens ------------------------------------------------------- */

static int confirm_screen(const char *dev)
{
    cls();

    title("Confirm installation");
    text(3, 2, "Everything on the target disk will be overwritten.");
    text(4, 2, "There is no undo.");
    text(6, 2, "Target disk:");

    mv(6, 20);
    fputs("\033[7m", stdout);
    fputs(dev, stdout);
    fputs("\033[0m", stdout);

    text(9, 2, "Type the device name to continue, or hit Enter to abort.");
    text(10, 2, "> ");

    char line[64];
    if (!fgets(line, sizeof line, stdin))
        return 0;
    /* strip trailing newline */
    size_t l = strlen(line);
    while (l && (line[l-1] == '\n' || line[l-1] == '\r'))
        line[--l] = 0;
    return strcmp(line, dev) == 0;
}

static void done_screen(void)
{
    cls();
    title("Installation complete");
    text(3, 2, "The disk now boots GNOS on its own.");
    text(5, 2, "Remove the CD and restart the machine (or power it off):");
    text(6, 2, "  reboot(RB_POWER_OFF) -- just cleanly halting QEMU here is fine.");
    text(8, 2, "Press any key to power off now.");

    /* one key, then off */
    struct termios t;
    tcgetattr(0, &t);
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    (void)getchar();
    tcsetattr(0, TCSANOW, &t);
}

static void die(const char *msg)
{
    fprintf(stderr, "installer: %s: %s\n", msg, strerror(errno));
    exit(1);
}

int main(void)
{
    struct termios saved;
    term_raw(0, &saved);

    /* ---- which disks are there --------------------------------------- */
    struct disk disks[MAX_DISKS];
    int n = 0;
    find_disks(disks, &n);
    if (!n) {
        cls();
        title("GNOS installer");
        text(3, 2, "No hard disks found.");
        text(4, 2, " (the ATA driver is there; nothing is)");
        (void)getchar();
        term_restore(0, &saved);
        return 1;
    }

    uint32_t sects = image_sectors();
    if (!sects) {
        cls();
        title("GNOS installer");
        text(3, 2, "/proc/boot/root.img is missing.");
        text(4, 2, "Is /proc mounted?");
        (void)getchar();
        term_restore(0, &saved);
        return 1;
    }

    /* ---- disk select --------------------------------------------------- */
    int sel = 0;
    for (;;) {
        cls();
        title("GNOS installer - select a target disk");
        hline(2);
        for (int i = 0; i < n; i++) {
            char line[80];
            snprintf(line, sizeof line,
                     "  %-8s %6llu MiB",
                     disks[i].name,
                     (unsigned long long)(disks[i].bytes / (1024 * 1024)));
            menu_item(4 + i, 0, line, i == sel);
        }
        text(11, 2, "Up/Down: select     Enter: install to this disk");
        text(12, 2, "q: quit (back to the shell)");

        int c = getchar();
        if (c == 'q')
            break;
        if (c == '\033') {             /* escape sequences: ESC [ A / B */
            getchar();
            switch (getchar()) {
            case 'A': if (sel > 0) sel--; break;
            case 'B': if (sel < n - 1) sel++; break;
            }
            continue;
        }
        if (c == '\n' || c == '\r') {
            struct disk *d = &disks[sel];
            /* ---- confirm ------------------------------------------------- */
            int ok = 0;
            {
                term_restore(0, &saved);
                ok = confirm_screen(d->name);
                term_raw(0, &saved);
            }
            if (!ok)
                continue;

            cls();
            title("Installing to /dev/sda (this takes a little while)");

            /* the image must fit past the 1 MiB limine gap */
            if ((uint64_t)sects + PART_START_SECT > d->bytes / ATA_SECTOR) {
                mv(4, 2);
                fputs("Target disk is smaller than the root image.", stdout);
                fflush(stdout);
                (void)getchar();
                continue;
            }

            /* ---- stage 1 + stage 2 in the gap -------------------------- */
            FILE *bsys = fopen("/boot/limine/limine.sys", "rb");
            if (!bsys)
                die("open /boot/limine/limine.sys");
            static uint8_t biosys[BUF_SECTS * ATA_SECTOR];
            long bsize = (long)fread(biosys, 1, sizeof biosys, bsys);
            fclose(bsys);
            if (bsize < ATA_SECTOR + ATA_SECTOR)
                die("limine.sys is too small to be real");

            /* partition: everything from LBA 2048 to the end of the image */
            if (write_mbr(d->fd, biosys, PART_START_SECT, sects) < 0)
                die("write MBR");

            /* stage 2: the rest of limine.sys, one sector per write, into
             * the gap the partition table entry left alone */
            {
                uint64_t pos = ATA_SECTOR;
                uint64_t lba = 1;
                while (pos < (uint64_t)bsize) {
                    uint8_t sec[ATA_SECTOR];
                    memset(sec, 0, sizeof sec);
                    uint32_t chunk = (uint32_t)((uint64_t)bsize - pos);
                    if (chunk > ATA_SECTOR)
                        chunk = ATA_SECTOR;
                    memcpy(sec, biosys + pos, chunk);
                    if (pwrite(d->fd, sec, ATA_SECTOR,
                               (off_t)(lba * ATA_SECTOR)) != ATA_SECTOR)
                        die("write limine stage 2");
                    pos += chunk;
                    lba++;
                }
            }

            /* ---- publish the partition, then copy the root --------------- */
            if (ioctl(d->fd, BLKRRPART, 0) < 0)
                die("BLKRRPART");

            char part[8];
            snprintf(part, sizeof part, "%s1", d->name);
            int pfd = open(part, O_RDWR);
            if (pfd < 0)
                die("open target partition");

            int imgfd = open("/proc/boot/root.img", O_RDONLY);
            if (imgfd < 0)
                die("open /proc/boot/root.img");

            int vres = copy_root(pfd, imgfd, (uint64_t)sects * ATA_SECTOR, 18);
            close(imgfd);
            close(pfd);
            if (vres == -3)
                die("verify: read-back mismatch");
            if (vres == -2)
                die("verify: root image truncated");
            if (vres)
                die("verify failed");

            mv(19, 0);
            fputs("\033[Kok.", stdout);
            fflush(stdout);

            (void)getchar();           /* pause, then the done screen */
            term_restore(0, &saved);
            done_screen();
            reboot(RB_POWER_OFF);
            return 0;                  /* unreachable unless poweroff fails */
        }
    }

    term_restore(0, &saved);
    return 0;
}