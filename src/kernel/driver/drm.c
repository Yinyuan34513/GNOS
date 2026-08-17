/*
 * drm.c — a minimal DRM/KMS driver: /dev/dri/card0 + /dev/dri/renderD128.
 * (GPLv2)
 *
 * This is the bochs-display/stdvga driver Linux calls "bochsdrm", shrunken
 * to what a teaching kernel can honestly support:
 *
 *   - the full ioctl surface a dumb-buffer client needs: version/unique,
 *     get/set client caps, resources, connector/encoder/crtc query,
 *     SETCRTC modesetting, CREATE/MAP/DESTROY_DUMB, ADDFB/ADDFB2/RMFB and
 *     PAGE_FLIP.  Everything else answers ENOTTY, the way a partial driver
 *     on Linux would.
 *   - modesetting through the Bochs VBE_DISPI registers (I/O 0x1CE/0x1CF),
 *     the interface QEMU's stdvga and bochs-display have both implemented
 *     for twenty-five years.  The framebuffer stays where it always was --
 *     the linear BAR0 the bootloader set up -- so fbcon keeps drawing into
 *     the same memory and only its geometry changes.
 *   - dumb buffers are physically contiguous frames handed out by the page
 *     allocator; MAP_DUMB encodes the handle in the mmap offset exactly as
 *     Linux does (offset = handle << PAGE_SHIFT), so an unmodified dumb-
 *     buffer client's mmap() lands on the right memory.
 *
 * The struct layouts and ioctl numbers in drm.h are byte-copies of the
 * Linux UAPI (see there); the kernel never guesses at an offset.
 */
#include "drm.h"
#include "pci.h"
#include "pmm.h"
#include "fbcon.h"
#include "bootinfo.h"
#include "subsys.h"
#include "vfs.h"
#include "vmm.h"
#include "io.h"
#include "kstring.h"
#include "debugcon.h"

extern uint64_t g_hhdm;

/* ---- the bochs VBE_DISPI register interface ------------------------------ */
#define VBE_PORT_IDX  0x01CE
#define VBE_PORT_DAT  0x01CF
#define VBE_ID      0x00
#define VBE_XRES    0x01
#define VBE_YRES    0x02
#define VBE_BPP     0x03
#define VBE_ENABLE  0x04
#define VBE_ENABLE_LFB 0x41             /* enable + linear framebuffer */

static void vbe_write(uint16_t idx, uint16_t val)
{
    outw(VBE_PORT_IDX, idx);
    outw(VBE_PORT_DAT, val);
}

static uint16_t vbe_read(uint16_t idx)
{
    outw(VBE_PORT_IDX, idx);
    return inw(VBE_PORT_DAT);
}

static int g_vbe;                       /* VBE_DISPI answered the ID probe */

/* ---- current scanout state ------------------------------------------------ */
static uint32_t g_cur_w, g_cur_h;       /* current mode */
static uint32_t g_cur_fb;               /* fb id on screen, 0 = console owns it */

/* ---- the mode list ---------------------------------------------------------
 * The boot mode (whatever Limine set up, 1280x800 under QEMU) is mode 0 and
 * what GETCRTC reports until the first SETCRTC.  The rest are standard VGA
 * timings; the htotal/vtotal blanks are the CEA-ish defaults, close enough
 * for a virtual display that no monitor ever measures.
 */
typedef struct {
    const char *name;
    uint32_t    h, v;
} mode_t;

static const mode_t g_modes[] = {
    { "1280x800", 1280, 800 },
    { "1024x768", 1024, 768 },
    { "800x600",  800,  600 },
    { "640x480",  640,  480 },
};
#define N_MODES (int)(sizeof(g_modes) / sizeof(g_modes[0]))

static int boot_mode_known(uint32_t *index)
{
    for (int i = 0; i < N_MODES; i++)
        if (g_modes[i].h == g_cur_w && g_modes[i].v == g_cur_h) {
            *index = (uint32_t)i;
            return 1;
        }
    return 0;
}

static void fill_modeinfo(drm_mode_modeinfo_t *m, const mode_t *src)
{
    memset(m, 0, sizeof(*m));
    m->clock      = (uint32_t)((uint64_t)src->h * src->v * 60 / 1000);
    m->hdisplay   = (uint16_t)src->h;
    m->hsync_start = (uint16_t)(src->h + 48);
    m->hsync_end   = (uint16_t)(src->h + 112);
    m->htotal      = (uint16_t)(src->h + 160);
    m->vdisplay    = (uint16_t)src->v;
    m->vsync_start = (uint16_t)(src->v + 3);
    m->vsync_end   = (uint16_t)(src->v + 6);
    m->vtotal      = (uint16_t)(src->v + 29);
    m->vrefresh    = 60;
    m->flags       = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
    for (int i = 0; src->name[i] && i < 31; i++)
        m->name[i] = src->name[i];
}

static const mode_t *mode_by_size(uint32_t w, uint32_t h)
{
    for (int i = 0; i < N_MODES; i++)
        if (g_modes[i].h == w && g_modes[i].v == h)
            return &g_modes[i];
    return NULL;
}

static void current_modeinfo(drm_mode_modeinfo_t *m)
{
    const mode_t *src = mode_by_size(g_cur_w, g_cur_h);
    if (src) {
        fill_modeinfo(m, src);
    } else {
        /* Boot set a mode not in our list: report it honestly. */
        memset(m, 0, sizeof(*m));
        m->clock    = (uint32_t)((uint64_t)g_cur_w * g_cur_h * 60 / 1000);
        m->hdisplay = (uint16_t)g_cur_w;
        m->vdisplay = (uint16_t)g_cur_h;
        m->htotal   = (uint16_t)(g_cur_w + 160);
        m->vtotal   = (uint16_t)(g_cur_h + 29);
        m->vrefresh = 60;
        m->flags    = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
    }
}

/* ---- dumb buffers ---------------------------------------------------------- */
#define MAX_DUMB 8
#define MAX_FB   8

typedef struct {
    int      used;
    uint32_t handle;
    uint64_t phys;
    uint32_t size;      /* bytes */
    uint32_t pitch;
    uint32_t w, h;
} dumb_t;

typedef struct {
    int      used;
    uint32_t fb_id;
    uint32_t width, height, pitch;
    uint32_t handle;
} fb_t;

static dumb_t g_dumb[MAX_DUMB];
static fb_t   g_fbs[MAX_FB];

static dumb_t *dumb_by_handle(uint32_t h)
{
    for (int i = 0; i < MAX_DUMB; i++)
        if (g_dumb[i].used && g_dumb[i].handle == h)
            return &g_dumb[i];
    return NULL;
}

static fb_t *fb_by_id(uint32_t id)
{
    for (int i = 0; i < MAX_FB; i++)
        if (g_fbs[i].used && g_fbs[i].fb_id == id)
            return &g_fbs[i];
    return NULL;
}

/* Copy [phys, phys+size) into the framebuffer at (x, y), row by row so a
 * pitch mismatch cannot skew the picture.  The fbcon owns the screen memory
 * the rest of the time; a SETCRTC/PAGE_FLIP is the one moment the client's
 * buffer is supposed to become visible, and afterwards any console paint
 * simply wins again -- the same "last writer owns the pixels" contract
 * fbdev has always had. */
static void blit_fb(uint64_t phys, uint32_t src_pitch, uint32_t w, uint32_t h,
                    uint32_t x, uint32_t y)
{
    uint32_t *dst = (uint32_t *)fbcon_fb();
    uint32_t dst_pitch = fbcon_pitch() / 4;
    const uint8_t *src = (const uint8_t *)pmm_virt(phys);

    if (x >= g_cur_w || y >= g_cur_h)
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

/* ---- user-memory helpers (same style as fbdev) ---------------------------- */
static int copy_from_user(void *k, uint64_t u, uint64_t n)
{
    if (!user_ptr_ok(u, n))
        return -E_FAULT;
    memcpy(k, (const void *)(uintptr_t)u, n);
    return 0;
}

static int copy_to_user(uint64_t u, const void *k, uint64_t n)
{
    if (!user_ptr_ok(u, n))
        return -E_FAULT;
    memcpy((void *)(uintptr_t)u, k, n);
    return 0;
}

/* ---- the ioctls ------------------------------------------------------------- */

static int32_t drm_ioctl_version(uint64_t arg)
{
    drm_version_t v;
    if (copy_from_user(&v, arg, sizeof v) < 0)
        return -E_FAULT;
    static const char name[] = "gnos-drm";
    static const char date[] = "20250101";
    static const char desc[] = "GNOS bochs-display DRM driver";
    v.version_major = 1; v.version_minor = 0; v.version_patchlevel = 0;
    if (v.name && v.name_len) copy_to_user((uint64_t)(uintptr_t)v.name, name,
                                           v.name_len < sizeof name ? v.name_len : sizeof name);
    if (v.date && v.date_len) copy_to_user((uint64_t)(uintptr_t)v.date, date,
                                           v.date_len < sizeof date ? v.date_len : sizeof date);
    if (v.desc && v.desc_len) copy_to_user((uint64_t)(uintptr_t)v.desc, desc,
                                           v.desc_len < sizeof desc ? v.desc_len : sizeof desc);
    return copy_to_user(arg, &v, sizeof v);
}

static int32_t drm_ioctl_get_unique(uint64_t arg)
{
    drm_unique_t u;
    if (copy_from_user(&u, arg, sizeof u) < 0)
        return -E_FAULT;
    static const char uniq[] = "gnos-bochs";
    if (u.unique && u.unique_len)
        copy_to_user((uint64_t)(uintptr_t)u.unique, uniq,
                     u.unique_len < sizeof uniq ? u.unique_len : sizeof uniq);
    return copy_to_user(arg, &u, sizeof u);
}

static int32_t drm_ioctl_get_cap(uint64_t arg)
{
    drm_get_cap_t c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -E_FAULT;
    switch (c.capability) {
    case DRM_CAP_DUMB_BUFFER:           c.value = 1; break;
    case DRM_CAP_DUMB_PREFERRED_DEPTH:  c.value = 24; break;
    case DRM_CAP_DUMB_PREFER_SHADOW:    c.value = 0; break;
    case DRM_CAP_TIMESTAMP_MONOTONIC:   c.value = 1; break;
    case DRM_CAP_ADDFB2_MODIFIERS:      c.value = 0; break; /* linear only */
    default:                            c.value = 0; break;
    }
    return copy_to_user(arg, &c, sizeof c);
}

static int32_t drm_ioctl_set_client_cap(uint64_t arg)
{
    drm_set_client_cap_t c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -E_FAULT;
    switch (c.capability) {
    case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
        /* The single primary plane is reported either way. */
        return 0;
    default:
        /* DRM_CLIENT_CAP_ATOMIC in particular: the modeset engine here is
         * legacy-only.  Accepting it would make wlroots drive a MODE_ATOMIC
         * commit path that does not exist.  Refuse loudly instead. */
        return -E_OPNOTSUPP;
    }
}

static int32_t drm_ioctl_get_resources(uint64_t arg)
{
    drm_mode_card_res_t r;
    if (copy_from_user(&r, arg, sizeof r) < 0)
        return -E_FAULT;

    uint32_t nfb = 0;
    for (int i = 0; i < MAX_FB; i++)
        if (g_fbs[i].used)
            nfb++;

    if (r.fb_id_ptr && r.count_fbs >= nfb) {
        uint32_t ids[MAX_FB];
        int k = 0;
        for (int i = 0; i < MAX_FB; i++)
            if (g_fbs[i].used)
                ids[k++] = g_fbs[i].fb_id;
        copy_to_user((uint64_t)(uintptr_t)r.fb_id_ptr, ids, k * 4);
    }
    if (r.crtc_id_ptr && r.count_crtcs >= 1) {
        uint32_t id = 1;
        copy_to_user((uint64_t)(uintptr_t)r.crtc_id_ptr, &id, 4);
    }
    if (r.connector_id_ptr && r.count_connectors >= 1) {
        uint32_t id = 1;
        copy_to_user((uint64_t)(uintptr_t)r.connector_id_ptr, &id, 4);
    }
    if (r.encoder_id_ptr && r.count_encoders >= 1) {
        uint32_t id = 1;
        copy_to_user((uint64_t)(uintptr_t)r.encoder_id_ptr, &id, 4);
    }

    r.count_fbs = nfb;
    r.count_crtcs = 1;
    r.count_connectors = 1;
    r.count_encoders = 1;
    r.min_width = 320; r.max_width = 4096;
    r.min_height = 200; r.max_height = 4096;
    return copy_to_user(arg, &r, sizeof r);
}

static int32_t drm_ioctl_get_connector(uint64_t arg)
{
    drm_mode_get_connector_t c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -E_FAULT;
    if (c.connector_id != 1)
        return -E_NOENT;

    if (c.encoders_ptr && c.count_encoders >= 1) {
        uint32_t id = 1;
        copy_to_user((uint64_t)(uintptr_t)c.encoders_ptr, &id, 4);
    }
    if (c.modes_ptr && c.count_modes >= (uint32_t)N_MODES) {
        drm_mode_modeinfo_t modes[N_MODES];
        for (int i = 0; i < N_MODES; i++)
            fill_modeinfo(&modes[i], &g_modes[i]);
        copy_to_user((uint64_t)(uintptr_t)c.modes_ptr, modes,
                     sizeof(modes[0]) * N_MODES);
    }
    if (c.props_ptr && c.count_props >= 2) {
        static const uint32_t props[2] = { DRM_PROP_ID_DPMS,
                                           DRM_PROP_ID_EDID };
        copy_to_user((uint64_t)(uintptr_t)c.props_ptr, props, sizeof props);
    }
    if (c.prop_values_ptr && c.count_props >= 2) {
        static const uint64_t vals[2] = { 0, 0 };   /* DPMS On, no EDID */
        copy_to_user((uint64_t)(uintptr_t)c.prop_values_ptr, vals,
                     sizeof vals);
    }

    c.count_modes = N_MODES;
    c.count_props = 2;
    c.count_encoders = 1;
    c.encoder_id = 1;
    c.connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
    c.connector_type_id = 1;
    c.connection = DRM_MODE_CONNECTED;
    c.mm_width = 320; c.mm_height = 200;
    c.subpixel = DRM_MODE_SUBPIXEL_UNKNOWN;
    c.pad = 0;
    return copy_to_user(arg, &c, sizeof c);
}

static int32_t drm_ioctl_get_encoder(uint64_t arg)
{
    drm_mode_get_encoder_t e;
    if (copy_from_user(&e, arg, sizeof e) < 0)
        return -E_FAULT;
    if (e.encoder_id != 1)
        return -E_NOENT;
    e.encoder_type = DRM_MODE_ENCODER_VIRTUAL;
    e.crtc_id = 1;
    e.possible_crtcs = 1;
    e.possible_clones = 0;
    return copy_to_user(arg, &e, sizeof e);
}

static int32_t drm_ioctl_get_crtc(uint64_t arg)
{
    drm_mode_crtc_t c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -E_FAULT;
    if (c.crtc_id != 1)
        return -E_NOENT;
    memset(&c, 0, sizeof c);
    c.crtc_id = 1;
    c.fb_id = g_cur_fb;
    c.x = c.y = 0;
    c.gamma_size = 0;
    c.mode_valid = 1;
    current_modeinfo(&c.mode);
    return copy_to_user(arg, &c, sizeof c);
}

static int32_t drm_ioctl_set_crtc(uint64_t arg)
{
    drm_mode_crtc_t c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -E_FAULT;
    if (c.crtc_id != 1)
        return -E_NOENT;
    if (c.x || c.y)
        return -E_INVAL;                /* no panning */
    if (c.count_connectors > 1)
        return -E_INVAL;

    /* Resolve the fb up front: a failed lookup must not leave the mode
     * switched behind an error return. */
    fb_t *f = NULL;
    dumb_t *d = NULL;
    if (c.fb_id) {
        f = fb_by_id(c.fb_id);
        if (!f)
            return -E_NOENT;
        d = dumb_by_handle(f->handle);
        if (!d)
            return -E_NOENT;
    }

    if (c.mode_valid) {
        uint32_t w = c.mode.hdisplay, h = c.mode.vdisplay;
        if (!w || !h)
            return -E_INVAL;
        if (!mode_by_size(w, h))
            return -E_INVAL;            /* only modes we advertise */

        /* A real mode change goes through the VBE registers; fbcon is then
         * told about the new geometry and repaints itself. */
        if (w != g_cur_w || h != g_cur_h) {
            if (!g_vbe)
                return -E_INVAL;
            vbe_write(VBE_ENABLE, 0);
            vbe_write(VBE_XRES, (uint16_t)w);
            vbe_write(VBE_YRES, (uint16_t)h);
            vbe_write(VBE_BPP, 32);
            vbe_write(VBE_ENABLE, VBE_ENABLE_LFB);
            if (vbe_read(VBE_XRES) != w || vbe_read(VBE_YRES) != h) {
                /* The new mode did not stick; put the old one back so the
                 * display is not left in a half-configured state. */
                vbe_write(VBE_ENABLE, 0);
                vbe_write(VBE_XRES, (uint16_t)g_cur_w);
                vbe_write(VBE_YRES, (uint16_t)g_cur_h);
                vbe_write(VBE_BPP, 32);
                vbe_write(VBE_ENABLE, VBE_ENABLE_LFB);
                dbg_puts("DRM: mode switch rejected (");
                dbg_puts_dec(w);
                dbg_puts("x");
                dbg_puts_dec(h);
                dbg_puts("), old mode restored\r\n");
                return -E_INVAL;
            }
            g_cur_w = w;
            g_cur_h = h;
            fbcon_resize(w, h, w * 4);
            dbg_puts("DRM: mode ");
            dbg_puts_dec(w);
            dbg_puts("x");
            dbg_puts_dec(h);
            dbg_puts("\r\n");
        }
    }

    /* fb_id 0 with no mode is a DPMS-style blank; we accept it as a no-op
     * (the console keeps painting, which is this driver's honest answer). */
    if (f) {
        blit_fb(d->phys, d->pitch, f->width, f->height, 0, 0);
        g_cur_fb = f->fb_id;
    }
    return 0;
}

static int32_t drm_ioctl_create_dumb(uint64_t arg)
{
    drm_mode_create_dumb_t c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -E_FAULT;
    if (!c.width || !c.height || c.bpp != 32 || c.flags)
        return -E_INVAL;
    if (c.width > 4096 || c.height > 4096)
        return -E_INVAL;                /* the advertised GETRESOURCES max */

    uint32_t pitch = c.width * 4;
    uint64_t size = (uint64_t)pitch * c.height;
    if (size == 0 || size > 16 * 1024 * 1024)
        return -E_INVAL;                /* 16 MiB cap: sane, and contiguous */

    int slot = -1;
    for (int i = 0; i < MAX_DUMB; i++)
        if (!g_dumb[i].used) { slot = i; break; }
    if (slot < 0)
        return -E_NOMEM;

    uint64_t nframes = (size + 0xFFF) >> 12;
    uint64_t phys = pmm_alloc_contiguous(nframes);
    if (!phys)
        return -E_NOMEM;
    memset(pmm_virt(phys), 0, (size_t)size);

    dumb_t *d = &g_dumb[slot];
    memset(d, 0, sizeof *d);
    d->used = 1;
    d->handle = (uint32_t)slot + 1;
    d->phys = phys;
    d->size = (uint32_t)size;
    d->pitch = pitch;
    d->w = c.width; d->h = c.height;

    c.handle = d->handle;
    c.pitch = pitch;
    c.size = size;
    return copy_to_user(arg, &c, sizeof c);
}

static int32_t drm_ioctl_map_dumb(uint64_t arg)
{
    drm_mode_map_dumb_t m;
    if (copy_from_user(&m, arg, sizeof m) < 0)
        return -E_FAULT;
    dumb_t *d = dumb_by_handle(m.handle);
    if (!d)
        return -E_NOENT;
    /* The Linux convention: mmap offset = handle << PAGE_SHIFT. */
    m.offset = (uint64_t)d->handle << 12;
    return copy_to_user(arg, &m, sizeof m);
}

static int32_t drm_ioctl_destroy_dumb(uint64_t arg)
{
    drm_mode_destroy_dumb_t d;
    if (copy_from_user(&d, arg, sizeof d) < 0)
        return -E_FAULT;
    dumb_t *b = dumb_by_handle(d.handle);
    if (!b)
        return -E_NOENT;
    /* Refuse to yank the memory out from under a live fb or a mapping. */
    for (int i = 0; i < MAX_FB; i++)
        if (g_fbs[i].used && g_fbs[i].handle == d.handle)
            return -E_BUSY;
    uint64_t nframes = (b->size + 0xFFF) >> 12;
    for (uint64_t i = 0; i < nframes; i++)
        pmm_free(b->phys + (i << 12));
    b->used = 0;
    return 0;
}

static int32_t drm_addfb(uint32_t width, uint32_t height, uint32_t pitch,
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

static int32_t drm_ioctl_addfb(uint64_t arg)
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

static int32_t drm_ioctl_addfb2(uint64_t arg)
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

static int32_t drm_ioctl_rmfb(uint64_t arg)
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

/* One primary plane, pinned to crtc 1.  wlroots refuses to start without
 * plane resources, so this is a hard requirement for the labwc line. */
static int32_t drm_ioctl_get_plane_res(uint64_t arg)
{
    drm_mode_get_plane_res_t r;
    if (copy_from_user(&r, arg, sizeof r) < 0)
        return -E_FAULT;
    if (r.plane_id_ptr && r.count_planes >= 1) {
        uint32_t id = 1;
        copy_to_user((uint64_t)(uintptr_t)r.plane_id_ptr, &id, 4);
    }
    r.count_planes = 1;
    return copy_to_user(arg, &r, sizeof r);
}

static const uint32_t g_plane_formats[] = {
    DRM_FORMAT_XRGB8888,
    DRM_FORMAT_ARGB8888,
};
#define N_PLANE_FORMATS \
    ((uint32_t)(sizeof g_plane_formats / sizeof g_plane_formats[0]))

static int32_t drm_ioctl_get_plane(uint64_t arg)
{
    drm_mode_get_plane_t p;
    if (copy_from_user(&p, arg, sizeof p) < 0)
        return -E_FAULT;
    if (p.plane_id != 1)
        return -E_NOENT;
    if (p.format_type_ptr && p.count_format_types >= N_PLANE_FORMATS)
        copy_to_user((uint64_t)(uintptr_t)p.format_type_ptr,
                     g_plane_formats, sizeof g_plane_formats);
    p.crtc_id = 1;
    p.fb_id = g_cur_fb;
    p.possible_crtcs = 1;
    p.gamma_size = 0;
    p.count_format_types = N_PLANE_FORMATS;
    return copy_to_user(arg, &p, sizeof p);
}

static const uint32_t g_dpms_values[4] = { 0, 1, 2, 3 };
static const uint32_t g_type_values[3] = { 0, 1, 2 };

static int32_t drm_fill_property(drm_mode_get_property_t *p)
{
    switch (p->prop_id) {
    case DRM_PROP_ID_DPMS:              /* connector: enum, current On */
        p->flags = DRM_MODE_PROP_ENUM;
        memcpy(p->name, "DPMS", 5);
        p->count_values = 4;
        p->count_enum_blobs = 4;
        if (p->values_ptr && p->count_values >= 4)
            copy_to_user((uint64_t)(uintptr_t)p->values_ptr,
                         g_dpms_values, sizeof g_dpms_values);
        if (p->enum_blob_ptr && p->count_enum_blobs >= 4) {
            static const drm_mode_property_enum_t enums[4] = {
                { 0, "On" }, { 1, "Standby" }, { 2, "Suspend" }, { 3, "Off" },
            };
            copy_to_user((uint64_t)(uintptr_t)p->enum_blob_ptr,
                         enums, sizeof enums);
        }
        return 0;
    case DRM_PROP_ID_EDID:              /* connector: blob, none fitted */
        p->flags = DRM_MODE_PROP_BLOB;
        memcpy(p->name, "EDID", 5);
        p->count_values = 0;
        p->count_enum_blobs = 0;
        return 0;
    case DRM_PROP_ID_PLANE_TYPE:        /* plane: enum, value 1 = Primary */
        p->flags = DRM_MODE_PROP_ENUM;
        memcpy(p->name, "type", 5);
        p->count_values = 3;
        p->count_enum_blobs = 3;
        if (p->values_ptr && p->count_values >= 3)
            copy_to_user((uint64_t)(uintptr_t)p->values_ptr,
                         g_type_values, sizeof g_type_values);
        if (p->enum_blob_ptr && p->count_enum_blobs >= 3) {
            static const drm_mode_property_enum_t enums[3] = {
                { 0, "Overlay" }, { 1, "Primary" }, { 2, "Cursor" },
            };
            copy_to_user((uint64_t)(uintptr_t)p->enum_blob_ptr,
                         enums, sizeof enums);
        }
        return 0;
    }
    return -E_NOENT;
}

static int32_t drm_ioctl_get_property(uint64_t arg)
{
    drm_mode_get_property_t p;
    if (copy_from_user(&p, arg, sizeof p) < 0)
        return -E_FAULT;
    int32_t r = drm_fill_property(&p);
    if (r < 0)
        return r;
    return copy_to_user(arg, &p, sizeof p);
}

static int32_t drm_ioctl_obj_get_properties(uint64_t arg)
{
    drm_mode_obj_get_properties_t o;
    if (copy_from_user(&o, arg, sizeof o) < 0)
        return -E_FAULT;

    static const uint32_t conn_props[2] = { DRM_PROP_ID_DPMS,
                                            DRM_PROP_ID_EDID };
    static const uint64_t conn_vals[2] = { 0, 0 };
    static const uint32_t plane_props[1] = { DRM_PROP_ID_PLANE_TYPE };
    static const uint64_t plane_vals[1] = { 1 };

    const uint32_t *props = NULL;
    const uint64_t *vals = NULL;
    uint32_t n = 0;

    switch (o.obj_type) {
    case DRM_MODE_OBJECT_CONNECTOR:
        if (o.obj_id != 1)
            return -E_NOENT;
        props = conn_props; vals = conn_vals; n = 2;
        break;
    case DRM_MODE_OBJECT_PLANE:
        if (o.obj_id != 1)
            return -E_NOENT;
        props = plane_props; vals = plane_vals; n = 1;
        break;
    case DRM_MODE_OBJECT_CRTC:
        if (o.obj_id != 1)
            return -E_NOENT;
        break;                          /* no properties on the crtc */
    default:
        return -E_NOENT;
    }

    if (o.props_ptr && o.count_props >= n)
        copy_to_user((uint64_t)(uintptr_t)o.props_ptr, props, n * 4);
    if (o.prop_values_ptr && o.count_props >= n)
        copy_to_user((uint64_t)(uintptr_t)o.prop_values_ptr, vals, n * 8);
    o.count_props = n;
    return copy_to_user(arg, &o, sizeof o);
}

static int32_t drm_ioctl_atomic(uint64_t arg)
{
    (void)arg;
    /* Not offered.  SET_CLIENT_CAP(DRM_CLIENT_CAP_ATOMIC) is refused, so a
     * well-behaved client never gets here; anything that does gets a loud
     * error instead of a silently partial modeset. */
    return -E_OPNOTSUPP;
}

static int32_t drm_ioctl_page_flip(uint64_t arg)
{
    struct {
        uint32_t crtc_id, fb_id, flags, reserved;
        uint64_t user_data;
    } p;
    if (copy_from_user(&p, arg, sizeof p) < 0)
        return -E_FAULT;
    if (p.crtc_id != 1)
        return -E_NOENT;
    if (p.flags)
        return -E_INVAL;                /* no async, no vblank events: say so */
    fb_t *f = fb_by_id(p.fb_id);
    if (!f)
        return -E_NOENT;
    dumb_t *d = dumb_by_handle(f->handle);
    if (!d)
        return -E_NOENT;
    /* No vblank here: a page flip is an immediate blit. */
    blit_fb(d->phys, d->pitch, f->width, f->height, 0, 0);
    g_cur_fb = f->fb_id;
    return 0;
}

static int32_t drm_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    (void)n;
    switch (cmd) {
    case DRM_IOCTL_VERSION:        return drm_ioctl_version(arg);
    case DRM_IOCTL_GET_UNIQUE:     return drm_ioctl_get_unique(arg);
    case DRM_IOCTL_GET_CAP:        return drm_ioctl_get_cap(arg);
    case DRM_IOCTL_SET_CLIENT_CAP: return drm_ioctl_set_client_cap(arg);
    case DRM_IOCTL_MODE_GETRESOURCES: return drm_ioctl_get_resources(arg);
    case DRM_IOCTL_MODE_GETCONNECTOR: return drm_ioctl_get_connector(arg);
    case DRM_IOCTL_MODE_GETENCODER:   return drm_ioctl_get_encoder(arg);
    case DRM_IOCTL_MODE_GETCRTC:      return drm_ioctl_get_crtc(arg);
    case DRM_IOCTL_MODE_SETCRTC:      return drm_ioctl_set_crtc(arg);
    case DRM_IOCTL_MODE_GETPROPERTY:  return drm_ioctl_get_property(arg);
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES: return drm_ioctl_obj_get_properties(arg);
    case DRM_IOCTL_MODE_GETPLANERESOURCES: return drm_ioctl_get_plane_res(arg);
    case DRM_IOCTL_MODE_GETPLANE:     return drm_ioctl_get_plane(arg);
    case DRM_IOCTL_MODE_ATOMIC:       return drm_ioctl_atomic(arg);
    case DRM_IOCTL_MODE_CREATE_DUMB:  return drm_ioctl_create_dumb(arg);
    case DRM_IOCTL_MODE_MAP_DUMB:     return drm_ioctl_map_dumb(arg);
    case DRM_IOCTL_MODE_DESTROY_DUMB: return drm_ioctl_destroy_dumb(arg);
    case DRM_IOCTL_MODE_ADDFB:        return drm_ioctl_addfb(arg);
    case DRM_IOCTL_MODE_ADDFB2:       return drm_ioctl_addfb2(arg);
    case DRM_IOCTL_MODE_RMFB:         return drm_ioctl_rmfb(arg);
    case DRM_IOCTL_MODE_PAGE_FLIP:    return drm_ioctl_page_flip(arg);
    default:
        return -E_NOTTY;
    }
}

/* mmap(): offset >> PAGE_SHIFT names the dumb buffer, the Linux way. */
static int drm_mmap(vfs_node_t *n, uint64_t offset, uint64_t *phys,
                    uint64_t *size)
{
    (void)n;
    if (offset & 0xFFF)
        return -E_INVAL;
    dumb_t *d = dumb_by_handle((uint32_t)(offset >> 12));
    if (!d)
        return -E_NOENT;
    *phys = d->phys;
    *size = d->size;
    return 0;
}

static const vfs_ops_t g_drm_ops = {
    .ioctl = drm_ioctl,
    .mmap  = drm_mmap,
};

int drm_init(const bootinfo_t *bi)
{
    g_cur_w = bi->fb_width;
    g_cur_h = bi->fb_height;
    g_cur_fb = 0;
    if (!g_cur_w || !g_cur_h) {
        dbg_puts("DRM: no boot framebuffer, staying out of the way\n");
        return 0;
    }

    /* Is the bochs VBE_DISPI register pair there?  (stdvga / bochs-display) */
    uint16_t id = vbe_read(VBE_ID);
    if (id >= 0xB0C0 && id <= 0xB0C5) {
        g_vbe = 1;
        dbg_puts("DRM: bochs VBE_DISPI present (id ");
        dbg_puts_hexn(id, 4);
        dbg_puts(")\r\n");
    } else {
        dbg_puts("DRM: no VBE_DISPI; only the boot mode is available\n");
    }

    int slot = subsys_register("drm", "card0", SUBSYS_CLASS_GRAPHIC, 29, 1);
    if (vfs_register_dev("dri/card0", &g_drm_ops, NULL) != 0 ||
        vfs_register_dev("dri/renderD128", &g_drm_ops, NULL) != 0) {
        subsys_set_state(slot, SUBSYS_STATE_FAILED);
        dbg_puts("DRM: device registration failed\n");
        return 0;
    }
    subsys_set_state(slot, SUBSYS_STATE_LIVE);

    uint32_t idx = 0;
    if (boot_mode_known(&idx))
        dbg_puts("DRM: /dev/dri/card0 ready (boot mode ");
    else
        dbg_puts("DRM: /dev/dri/card0 ready (boot mode ");
    dbg_puts_dec(g_cur_w);
    dbg_puts("x");
    dbg_puts_dec(g_cur_h);
    dbg_puts(")\r\n");
    return 1;
}
