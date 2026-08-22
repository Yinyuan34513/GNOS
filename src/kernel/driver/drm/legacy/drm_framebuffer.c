/*
 * drm_framebuffer.c — fb objects (ADDFB/ADDFB2/RMFB) and the scanout blit.
 * (GPLv2)
 *
 * An fb is a view onto a dumb buffer: it pins the buffer's physical memory
 * and remembers the geometry a client asked to scan out.  blit_fb() is the
 * one moment a client's buffer is supposed to become visible; afterwards any
 * console paint simply wins again -- the same "last writer owns the pixels"
 * contract fbdev has always had.
 */
#include "drm_internal.h"
#include "pmm.h"
#include "fbcon.h"
#include "kstring.h"

fb_t g_fbs[MAX_FB];

fb_t *fb_by_id(uint32_t id)
{
    for (int i = 0; i < MAX_FB; i++)
        if (g_fbs[i].used && g_fbs[i].fb_id == id)
            return &g_fbs[i];
    return NULL;
}

/* Copy [phys, phys+size) into the framebuffer at (x, y), row by row so a
 * pitch mismatch cannot skew the picture. */
void blit_fb(uint64_t phys, uint32_t src_pitch, uint32_t w, uint32_t h,
             uint32_t x, uint32_t y)
{
    blit_fb_virt((const void *)(uintptr_t)pmm_virt(phys), src_pitch, w, h,
                 x, y);
}

/* Same scanout copy, but the source is a kernel virtual address.  The
 * ported (atomic) DRM stack allocates dumb-buffer backing from the kernel
 * heap, which has no physical address to hand to blit_fb(); its page-flip
 * helper lands here instead. */
void blit_fb_virt(const void *src_v, uint32_t src_pitch, uint32_t w,
                  uint32_t h, uint32_t x, uint32_t y)
{
    uint32_t *dst = (uint32_t *)fbcon_fb();
    uint32_t dst_pitch = fbcon_pitch() / 4;
    const uint8_t *src = (const uint8_t *)src_v;

    if (!src || x >= g_cur_w || y >= g_cur_h)
        return;

    /* A buffer larger than the scanout is clipped to the visible
     * rectangle, never dropped whole. */
    if (w > g_cur_w - x)
        w = g_cur_w - x;
    if (h > g_cur_h - y)
        h = g_cur_h - y;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t *d = dst + (y + row) * dst_pitch + x;
        const uint32_t *s = (const uint32_t *)(src + row * src_pitch);
        for (uint32_t col = 0; col < w; col++)
            d[col] = s[col];
    }
}

int32_t drm_addfb(uint32_t width, uint32_t height, uint32_t pitch,
                  uint32_t bpp, uint32_t depth, uint32_t handle,
                  uint32_t *out_id)
{
    if (!width || !height || width > 4096 || height > 4096 ||
        pitch < width * 4 || bpp != 32 || depth != 24)
        return -E_INVAL;
    dumb_t *d = dumb_by_handle(handle);
    if (!d)
        return -E_NOENT;
    if (pitch > d->pitch || (uint64_t)pitch * height > d->size)
        return -E_INVAL;

    int slot = -1;
    for (int i = 0; i < MAX_FB; i++)
        if (!g_fbs[i].used) { slot = i; break; }
    if (slot < 0)
        return -E_NOMEM;

    fb_t *f = &g_fbs[slot];
    memset(f, 0, sizeof *f);
    f->used = 1;
    f->fb_id = (uint32_t)slot + 1;
    f->width = width; f->height = height; f->pitch = pitch;
    f->handle = handle;
    *out_id = f->fb_id;
    return 0;
}

int32_t drm_ioctl_getfb(uint64_t arg)
{
    drm_mode_fb_cmd_t c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -E_FAULT;
    fb_t *f = fb_by_id(c.fb_id);
    if (!f)
        return -E_NOENT;
    c.width = f->width;
    c.height = f->height;
    c.pitch = f->pitch;
    c.bpp = 32;
    c.depth = 24;
    c.handle = f->handle;
    return copy_to_user(arg, &c, sizeof c);
}

int32_t drm_ioctl_addfb(uint64_t arg)
{
    drm_mode_fb_cmd_t c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -E_FAULT;
    int32_t r = drm_addfb(c.width, c.height, c.pitch, c.bpp, c.depth,
                          c.handle, &c.fb_id);
    if (r < 0)
        return r;
    return copy_to_user(arg, &c, sizeof c);
}

int32_t drm_ioctl_addfb2(uint64_t arg)
{
    drm_mode_fb_cmd2_t c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -E_FAULT;
    if (c.flags & ~DRM_MODE_FB_MODIFIERS)
        return -E_INVAL;
    if (c.handles[1] || c.handles[2] || c.handles[3] ||
        c.pitches[1] || c.pitches[2] || c.pitches[3] ||
        c.offsets[1] || c.offsets[2] || c.offsets[3])
        return -E_INVAL;                /* single-plane, no offsets */
    if (c.offsets[0])
        return -E_INVAL;
    if ((c.flags & DRM_MODE_FB_MODIFIERS) &&
        (c.modifier[0] != DRM_FORMAT_MOD_LINEAR ||
         c.modifier[1] || c.modifier[2] || c.modifier[3]))
        return -E_OPNOTSUPP;            /* linear scanout only */
    if (c.pixel_format != DRM_FORMAT_XRGB8888 &&
        c.pixel_format != DRM_FORMAT_ARGB8888)
        return -E_INVAL;
    int32_t r = drm_addfb(c.width, c.height, c.pitches[0], 32, 24,
                          c.handles[0], &c.fb_id);
    if (r < 0)
        return r;
    return copy_to_user(arg, &c, sizeof c);
}

int32_t drm_ioctl_rmfb(uint64_t arg)
{
    uint32_t id;
    if (copy_from_user(&id, arg, sizeof id) < 0)
        return -E_FAULT;
    fb_t *f = fb_by_id(id);
    if (!f)
        return -E_NOENT;
    if (g_cur_fb == id)
        g_cur_fb = 0;
    f->used = 0;
    return 0;
}

int32_t drm_ioctl_closefb(uint64_t arg)
{
    /* drmModeCloseFB: newer libdrm closes fbs with MODE_CLOSEFB instead of
     * MODE_RMFB (wlroots' drm_fb_destroy calls CloseFB and falls back to
     * RmFB only on -EINVAL).  The two are the same teardown here. */
    drm_mode_closefb_t c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -E_FAULT;
    fb_t *f = fb_by_id(c.fb_id);
    if (!f)
        return -E_NOENT;
    if (g_cur_fb == c.fb_id)
        g_cur_fb = 0;
    f->used = 0;
    return 0;
}
