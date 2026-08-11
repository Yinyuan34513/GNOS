/*
 * fbtest.c — headless assertions for /dev/fb0. (GPLv2)
 *
 * A framebuffer is the one subsystem you are most tempted to "test" by
 * looking at the screen, which is exactly what a `make test` run cannot do.
 * So this asserts the parts that a human eye would not catch anyway: that the
 * geometry the ioctls report is self-consistent, that mmap really hands back
 * the hardware (a byte written through the mapping is visible through read(),
 * and the other way round), and that the whole mapping is present rather than
 * just its first page -- which is the failure mode of a kernel that maps a
 * device mmap with a single vmm_map() call.
 *
 * Everything it draws is restored before it exits, so the boot log survives.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

/* <linux/fb.h> is not in musl's include tree; these are its definitions. */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602

struct fb_bitfield { unsigned int offset, length, msb_right; };

struct fb_var_screeninfo {
    unsigned int xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    unsigned int bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    unsigned int nonstd, activate, height, width, accel_flags, pixclock;
    unsigned int left_margin, right_margin, upper_margin, lower_margin;
    unsigned int hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    unsigned int reserved[4];
};

struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    unsigned int  smem_len, type, type_aux, visual;
    unsigned short xpanstep, ypanstep, ywrapstep;
    unsigned int line_length;
    unsigned long mmio_start;
    unsigned int mmio_len, accel;
    unsigned short capabilities, reserved[2];
};

static int fails = 0;

static void ok(const char *name, int cond)
{
    printf("fbtest: %s: %s\n", name, cond ? "ok" : "FAIL");
    if (!cond)
        fails++;
}

int main(void)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;

    /* The struct sizes are part of the ABI: if these are wrong every field
     * the kernel fills lands in the wrong place and the rest of this test
     * would be checking garbage against garbage. */
    ok("sizeof fb_var_screeninfo == 160", sizeof(var) == 160);
    ok("sizeof fb_fix_screeninfo == 80",  sizeof(fix) == 80);

    int fd = open("/dev/fb0", O_RDWR);
    ok("open /dev/fb0", fd >= 0);
    if (fd < 0) {
        printf("  open failed: %s\n", strerror(errno));
        printf("fbtest: %d failure(s)\n", fails);
        return fails ? 1 : 0;
    }

    memset(&fix, 0, sizeof fix);
    ok("FBIOGET_FSCREENINFO", ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0);
    memset(&var, 0, sizeof var);
    ok("FBIOGET_VSCREENINFO", ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0);

    printf("fbtest: %ux%u %ubpp, pitch %u, %u bytes at phys 0x%lx (\"%s\")\n",
           var.xres, var.yres, var.bits_per_pixel, fix.line_length,
           fix.smem_len, fix.smem_start, fix.id);

    ok("geometry is non-zero", var.xres > 0 && var.yres > 0);
    ok("32 bits per pixel", var.bits_per_pixel == 32);
    ok("pitch covers a scanline", fix.line_length >= var.xres * 4);
    ok("smem_len == pitch * height",
       fix.smem_len == fix.line_length * var.yres);
    /* XRGB8888, which is what every drawing routine here assumes. */
    ok("red at bit 16",   var.red.offset == 16 && var.red.length == 8);
    ok("green at bit 8",  var.green.offset == 8 && var.green.length == 8);
    ok("blue at bit 0",   var.blue.offset == 0 && var.blue.length == 8);

    /* Asking for a mode we are already in must succeed; asking for a
     * different pixel format must not silently "succeed". */
    struct fb_var_screeninfo same = var;
    ok("FBIOPUT_VSCREENINFO (same mode)",
       ioctl(fd, FBIOPUT_VSCREENINFO, &same) == 0);
    struct fb_var_screeninfo other = var;
    other.bits_per_pixel = 16;
    ok("FBIOPUT_VSCREENINFO (16bpp) is refused",
       ioctl(fd, FBIOPUT_VSCREENINFO, &other) != 0);

    unsigned char *fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0);
    ok("mmap /dev/fb0", fb != MAP_FAILED);
    if (fb == MAP_FAILED) {
        printf("  mmap failed: %s\n", strerror(errno));
        close(fd);
        printf("fbtest: %d failure(s)\n", fails);
        return 1;
    }

    /* Pick a spot in the bottom-right corner, well clear of the boot text,
     * and remember what was there. */
    unsigned x = var.xres - 8, y = var.yres - 8;
    off_t off = (off_t)y * fix.line_length + (off_t)x * 4;
    unsigned int original;
    memcpy(&original, fb + off, 4);

    /* mmap and read() must be two views of one buffer, in both directions.
     *
     * Every transfer is completed *before* the first ok(), because ok()
     * prints, printing goes to the console, and the console is drawing on
     * this very framebuffer.  A line of output at the bottom of the screen
     * scrolls it and overwrites the pixel under test.  That is not a bug --
     * it is the documented "last writer wins" behaviour of sharing /dev/fb0
     * with fbcon -- but a test that reads the pixel back after printing is
     * racing the console and will fail for the wrong reason. */
    unsigned int magenta = 0x00FF00FF;
    memcpy(fb + off, &magenta, 4);
    unsigned int through_read = 0;
    ssize_t rn = pread(fd, &through_read, 4, off);
    ok("pread at the mapped offset", rn == 4);
    ok("read() sees what mmap wrote", through_read == magenta);

    unsigned int cyan = 0x0000FFFF;
    ssize_t wn = pwrite(fd, &cyan, 4, off);
    unsigned int through_mmap = 0;
    memcpy(&through_mmap, fb + off, 4);
    ok("pwrite at the mapped offset", wn == 4);
    ok("mmap sees what write() wrote", through_mmap == cyan);

    /* The *last* page of the mapping has to be mapped too.  A kernel that
     * maps only the first page of a device mmap passes every test above and
     * faults here. */
    off_t last = (off_t)fix.smem_len - 4;
    unsigned int saved_last;
    memcpy(&saved_last, fb + last, 4);
    unsigned int probe = 0x00123456;
    memcpy(fb + last, &probe, 4);
    unsigned int read_last = 0;
    ssize_t ln = pread(fd, &read_last, 4, last);
    ok("pread at the last mapped byte", ln == 4);
    ok("the whole span is mapped, not just page 0", read_last == probe);
    memcpy(fb + last, &saved_last, 4);

    /* A read that starts past the end is EOF, not an error. */
    char dummy;
    ok("read past the end is EOF",
       pread(fd, &dummy, 1, (off_t)fix.smem_len + 4096) == 0);

    memcpy(fb + off, &original, 4);
    munmap(fb, fix.smem_len);
    close(fd);

    printf("fbtest: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
