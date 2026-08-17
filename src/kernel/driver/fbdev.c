/*
 * fbdev.c — /dev/fb0, a Linux-compatible framebuffer device. (GPLv2)
 *
 * The bootloader hands us one linear framebuffer and fbcon has already
 * claimed it as the boot console.  This file publishes the *same* memory a
 * second time, as a file under /dev, with the ioctls and struct layouts from
 * <linux/fb.h>.  A program that knows how to draw on Linux -- fbset, an SDL
 * fbcon backend, a splash writer -- therefore works here unmodified: it
 * opens /dev/fb0, asks for the geometry, mmaps the scanlines and writes
 * pixels.  Nothing arbitrates between the console and the mapping; on Linux
 * nothing does either, and the last writer wins.
 *
 * The structs below are byte-for-byte copies of the kernel ABI, not a
 * convenient subset.  That matters more than it looks: user space computes
 * the offset of `smem_len` or `line_length` from its own copy of the header,
 * so a field dropped here is not a missing feature, it is silent corruption.
 * The self-test asserts the two sizes for exactly that reason.
 */
#include "fbdev.h"

#include <stdint.h>

#include "bootinfo.h"
#include "debugcon.h"
#include "fbcon.h"
#include "gfx.h"
#include "kstring.h"
#include "subsys.h"
#include "vfs.h"
#include "vmm.h"

/* The higher-half direct map: framebuffer MMIO is reachable at phys+g_hhdm,
 * and we have to undo that to hand a *physical* span back to mmap(). */
extern uint64_t g_hhdm;

/* ---- the <linux/fb.h> ABI --------------------------------------------- */

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606
#define FBIOBLANK           0x4611

/* One colour channel's position and width inside a pixel. */
typedef struct {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
} fb_bitfield_t;

/* 160 bytes.  Order and padding follow struct fb_var_screeninfo exactly. */
typedef struct {
    uint32_t      xres, yres;
    uint32_t      xres_virtual, yres_virtual;
    uint32_t      xoffset, yoffset;
    uint32_t      bits_per_pixel;
    uint32_t      grayscale;
    fb_bitfield_t red, green, blue, transp;
    uint32_t      nonstd;
    uint32_t      activate;
    uint32_t      height, width;      /* physical mm, -1 when unknown */
    uint32_t      accel_flags;
    uint32_t      pixclock;
    uint32_t      left_margin, right_margin, upper_margin, lower_margin;
    uint32_t      hsync_len, vsync_len;
    uint32_t      sync, vmode, rotate, colorspace;
    uint32_t      reserved[4];
} fb_var_screeninfo_t;

/* 80 bytes, including the two bytes of tail padding the C ABI inserts after
 * the uint16_t.  struct fb_fix_screeninfo is 80 bytes on x86-64 too. */
typedef struct {
    char     id[16];
    uint64_t smem_start;      /* physical address of the pixels */
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
} fb_fix_screeninfo_t;

#define FB_TYPE_PACKED_PIXELS  0
#define FB_VISUAL_TRUECOLOR    2

/* ---- device state ------------------------------------------------------ */

static struct {
    uint8_t *base;      /* virtual, via the HHDM */
    uint64_t phys;      /* what mmap() hands out */
    uint64_t len;       /* pitch * height, the whole visible span */
    uint32_t w, h;
    uint32_t pitch;     /* bytes per scanline */
    uint32_t bpp;       /* bits per pixel; we only support 32 */
    int      live;
} g_fb;

/* ---- read/write: the framebuffer as a flat, fixed-size file ------------ */

static int32_t fb_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n;
    if (!g_fb.live)
        return -E_NODEV;
    if (off >= g_fb.len)
        return 0;                       /* EOF, like a regular short file */
    if (off + len > g_fb.len)
        len = (uint32_t)(g_fb.len - off);
    memcpy(buf, g_fb.base + off, len);
    return (int32_t)len;
}

static int32_t fb_write(vfs_node_t *n, uint64_t off, const void *buf,
                        uint32_t len)
{
    (void)n;
    if (!g_fb.live)
        return -E_NODEV;
    if (off >= g_fb.len)
        return -E_NOSPC;                /* a framebuffer cannot grow */
    if (off + len > g_fb.len)
        len = (uint32_t)(g_fb.len - off);
    memcpy(g_fb.base + off, buf, len);
    return (int32_t)len;
}

/* ---- ioctl ------------------------------------------------------------- */

static void fill_var(fb_var_screeninfo_t *v)
{
    /* The DRM driver can switch modes underneath us; always report the
     * mode that is on screen *now*, not the boot one. */
    fbcon_geometry(&g_fb.w, &g_fb.h, &g_fb.pitch);
    g_fb.len = (uint64_t)g_fb.pitch * g_fb.h;

    memset(v, 0, sizeof(*v));
    v->xres = v->xres_virtual = g_fb.w;
    v->yres = v->yres_virtual = g_fb.h;
    v->bits_per_pixel = g_fb.bpp;
    /* XRGB8888, which is what Limine gives us and what gfx writes. */
    v->red.offset   = 16; v->red.length   = 8;
    v->green.offset =  8; v->green.length = 8;
    v->blue.offset  =  0; v->blue.length  = 8;
    v->transp.offset = 0; v->transp.length = 0;
    v->height = v->width = (uint32_t)-1;   /* physical size unknown */
    v->vmode  = 0;                          /* FB_VMODE_NONINTERLACED */
}

static void fill_fix(fb_fix_screeninfo_t *f)
{
    memset(f, 0, sizeof(*f));
    strncpy(f->id, "GNOS FB", sizeof(f->id));
    f->smem_start  = g_fb.phys;
    f->smem_len    = (uint32_t)g_fb.len;
    f->type        = FB_TYPE_PACKED_PIXELS;
    f->visual      = FB_VISUAL_TRUECOLOR;
    f->line_length = g_fb.pitch;
    /* No hardware panning and no wrap: the scanout base is fixed. */
    f->xpanstep = f->ypanstep = f->ywrapstep = 0;
}

static int32_t fb_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    (void)n;
    if (!g_fb.live)
        return -E_NODEV;

    switch (cmd) {
    case FBIOGET_VSCREENINFO: {
        fb_var_screeninfo_t v;
        if (!user_ptr_ok(arg, sizeof(v)))
            return -E_FAULT;
        fill_var(&v);
        memcpy((void *)(uintptr_t)arg, &v, sizeof(v));
        return 0;
    }
    case FBIOGET_FSCREENINFO: {
        fb_fix_screeninfo_t f;
        if (!user_ptr_ok(arg, sizeof(f)))
            return -E_FAULT;
        fill_fix(&f);
        memcpy((void *)(uintptr_t)arg, &f, sizeof(f));
        return 0;
    }
    case FBIOPUT_VSCREENINFO: {
        /* We cannot reprogram the bootloader's mode, so the only request we
         * can honour is "keep what you have".  Refusing anything else with
         * EINVAL is what a fixed-mode Linux driver does, and it is what a
         * well-behaved client probes for. */
        fb_var_screeninfo_t want, have;
        if (!user_ptr_ok(arg, sizeof(want)))
            return -E_FAULT;
        memcpy(&want, (const void *)(uintptr_t)arg, sizeof(want));
        fill_var(&have);
        if (want.xres != have.xres || want.yres != have.yres ||
            want.bits_per_pixel != have.bits_per_pixel)
            return -E_INVAL;
        return 0;
    }
    case FBIOPAN_DISPLAY: {
        /* Only the (0,0) origin exists; anything else would show garbage. */
        fb_var_screeninfo_t want;
        if (!user_ptr_ok(arg, sizeof(want)))
            return -E_FAULT;
        memcpy(&want, (const void *)(uintptr_t)arg, sizeof(want));
        if (want.xoffset != 0 || want.yoffset != 0)
            return -E_INVAL;
        return 0;
    }
    case FBIOBLANK:
        /* No DPMS on a bootloader framebuffer.  Succeeding for "unblank"
         * and refusing the rest keeps callers from looping. */
        return arg == 0 ? 0 : -E_INVAL;
    default:
        return -E_NOTTY;
    }
}

/* ---- mmap -------------------------------------------------------------- */

static int fb_mmap(vfs_node_t *n, uint64_t offset, uint64_t *phys,
                   uint64_t *size)
{
    (void)n;
    if (!g_fb.live)
        return -E_NODEV;
    if (offset != 0)
        return -E_INVAL;                /* one framebuffer, one window */
    fbcon_geometry(&g_fb.w, &g_fb.h, &g_fb.pitch);
    *phys = g_fb.phys;
    *size = (uint64_t)g_fb.pitch * g_fb.h;
    return 0;
}

static const vfs_ops_t g_fb_ops = {
    .read  = fb_read,
    .write = fb_write,
    .ioctl = fb_ioctl,
    .mmap  = fb_mmap,
};

/* ---- bring-up ---------------------------------------------------------- */

gfx_surface_t fbdev_screen(void)
{
    /* gfx treats a NULL base as "draw nothing", so a machine that booted
     * without a framebuffer needs no special case at the call sites. */
    return gfx_surface(g_fb.live ? g_fb.base : 0, g_fb.w, g_fb.h,
                       g_fb.pitch, 4);
}

int fbdev_init(const bootinfo_t *bi)
{
    int slot = subsys_register("fb", "fb0", SUBSYS_CLASS_GRAPHIC, 29, 0);

    if (!bi || !bi->fb_addr || bi->fb_width == 0 || bi->fb_height == 0) {
        dbg_puts("FBDEV: no linear framebuffer, /dev/fb0 not created\n");
        subsys_set_state(slot, SUBSYS_STATE_FAILED);
        return 0;
    }
    if (bi->fb_bpp != 32) {
        /* Every primitive here writes 32-bit words.  Silently drawing into a
         * 16-bpp mode would corrupt the console instead of failing. */
        dbg_puts("FBDEV: unsupported bpp, /dev/fb0 not created\n");
        subsys_set_state(slot, SUBSYS_STATE_FAILED);
        return 0;
    }

    g_fb.base  = (uint8_t *)bi->fb_addr;
    g_fb.phys  = bi->fb_addr - g_hhdm;
    g_fb.w     = bi->fb_width;
    g_fb.h     = bi->fb_height;
    g_fb.pitch = bi->fb_pitch;
    g_fb.bpp   = bi->fb_bpp;
    g_fb.len   = (uint64_t)bi->fb_pitch * bi->fb_height;
    g_fb.live  = 1;

    /* vfs_register_dev() answers 0 for success and a negative errno
     * otherwise, so this is a != test and not a truth test. */
    if (vfs_register_dev("fb0", &g_fb_ops, 0) != 0) {
        dbg_puts("FBDEV: /dev/fb0 registration failed\n");
        g_fb.live = 0;
        subsys_set_state(slot, SUBSYS_STATE_FAILED);
        return 0;
    }
    subsys_set_state(slot, SUBSYS_STATE_LIVE);

    dbg_puts("FBDEV: /dev/fb0 ");
    dbg_puts_dec(g_fb.w);
    dbg_puts("x");
    dbg_puts_dec(g_fb.h);
    dbg_puts("x");
    dbg_puts_dec(g_fb.bpp);
    dbg_puts(" pitch=");
    dbg_puts_dec(g_fb.pitch);
    dbg_puts(" phys=");
    dbg_puts_hex(g_fb.phys);
    dbg_puts("\n");
    return 1;
}

/* ---- self test --------------------------------------------------------- */

void fbdev_self_test(void)
{
    int ok = 1;

    /* A wrong struct size is the one failure user space cannot diagnose, so
     * check it before anything that depends on it. */
    if (sizeof(fb_var_screeninfo_t) != 160 || sizeof(fb_fix_screeninfo_t) != 80)
        ok = 0;

    if (ok && !g_fb.live) {
        /* Not an error on a headless machine, but say so rather than
         * reporting a pass we never actually made. */
        dbg_puts("FBDEV: self test skipped (no framebuffer)\n");
        return;
    }

    /* Work in a 16x16 corner and put back exactly what was there, so the
     * boot console the user is reading survives the test. */
    enum { BOX = 16 };
    uint32_t saved[BOX * BOX];
    uint32_t stride_px = g_fb.pitch / 4;

    if (ok && (g_fb.w < BOX || g_fb.h < BOX))
        ok = 0;

    if (ok) {
        for (int y = 0; y < BOX; y++)
            for (int x = 0; x < BOX; x++)
                saved[y * BOX + x] =
                    ((uint32_t *)g_fb.base)[(uint64_t)y * stride_px + x];
    }

    /* 1. gfx writes land where read() can see them. */
    if (ok) {
        gfx_surface_t s = fbdev_screen();
        uint32_t want = gfx_rgb(0x12, 0x34, 0x56);
        gfx_putpixel(&s, 3, 2, want);

        uint32_t got = 0;
        uint64_t off = (uint64_t)2 * g_fb.pitch + 3 * 4;
        if (fb_read(0, off, &got, 4) != 4 || got != want)
            ok = 0;
    }

    /* 2. write() lands where gfx can see it -- the same path in reverse. */
    if (ok) {
        uint32_t put = gfx_rgb(0x65, 0x43, 0x21);
        uint64_t off = (uint64_t)5 * g_fb.pitch + 7 * 4;
        if (fb_write(0, off, &put, 4) != 4)
            ok = 0;
        else if (((uint32_t *)g_fb.base)[(uint64_t)5 * stride_px + 7] != put)
            ok = 0;
    }

    /* 3. Reads past the end are EOF, not faults, and writes cannot grow it. */
    if (ok) {
        uint32_t junk;
        if (fb_read(0, g_fb.len, &junk, 4) != 0)
            ok = 0;
        if (fb_write(0, g_fb.len, &junk, 4) != -E_NOSPC)
            ok = 0;
    }

    /* 4. The geometry the ioctls report matches the memory we handed out. */
    if (ok) {
        fb_fix_screeninfo_t f;
        fb_var_screeninfo_t v;
        fill_fix(&f);
        fill_var(&v);
        if (f.line_length != g_fb.pitch || f.smem_len != g_fb.len ||
            v.xres != g_fb.w || v.yres != g_fb.h ||
            f.smem_start != g_fb.phys)
            ok = 0;
    }

    /* 5. mmap hands back a physical span, not the HHDM alias. */
    if (ok) {
        uint64_t p = 0, sz = 0;
        if (fb_mmap(0, 0, &p, &sz) != 0 || p != g_fb.phys ||
            sz != (uint64_t)g_fb.pitch * g_fb.h)
            ok = 0;
        if (p >= g_hhdm)               /* a virtual address leaked out */
            ok = 0;
    }

    if (ok) {
        for (int y = 0; y < BOX; y++)
            for (int x = 0; x < BOX; x++)
                ((uint32_t *)g_fb.base)[(uint64_t)y * stride_px + x] =
                    saved[y * BOX + x];
    }

    dbg_puts(ok ? "FBDEV: self test ok\n" : "FBDEV: self test FAILED\n");
}
