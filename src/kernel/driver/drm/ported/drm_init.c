/*
 *
 *      drm_init.c
 *      DRM subsystem initialization entry point
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *      Ported from Uinxed-Kernel (OpenXJ380/Uinxed-Kernel).  See README.md.
 *
 *  Creates a singleton DRM device, registers it, and exposes
 *  /dev/dri/card0 via devtmpfs. Designed to be called once from
 *  kernel_entry() after VFS/devtmpfs are available.
 *
 */

#include "drm_devtmpfs.h"  /* device/class/devtmpfs shim -> GNOS VFS */
#include "drm.h"
#include "drm_device.h"
#include "drm_fourcc.h"
#include "drm_init.h"
#include "drm_mode.h"
#include "drm_print.h"
#include "vfs.h"
#include "drm_vsnprintf.h"
#include <stddef.h>
#include <stdint.h>
#include "kstring.h"
#include "heap.h"
#define DRM_WAIT_SLEEP 5 /* proc.h WAIT_SLEEP */
/* proc.h: kthread_create returns proc_t*, sched_block_timeout takes
 * wait_reason_t (WAIT_SLEEP == 5 there).  Match those signatures exactly. */
typedef struct { int _; } proc_t_fwd;
void sched_block_timeout(uint32_t why, uint64_t ticks);

/* GNOS always builds the DRM core (the Uinxed build gate has no GNOS
 * counterpart). */
#ifndef CONFIG_DRM
#define CONFIG_DRM 1
#endif

extern int                      drm_vblank_init(struct drm_device *dev, unsigned int num_crtcs);
extern struct drm_display_mode *drm_mode_create(struct drm_device *dev);
extern void                     drm_mode_probed_add(struct drm_connector *connector, struct drm_display_mode *mode);

/* ------------------------------------------------------------------ */
/* Global DRM device list (replaces singleton)                         */
/* ------------------------------------------------------------------ */

#define DRM_MAX_DEVICES 16

static struct drm_device *drm_device_list[DRM_MAX_DEVICES];
static spinlock_t         drm_device_list_lock = {0};

void drm_device_list_add(struct drm_device *dev)
{
    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        if (!drm_device_list[i]) {
            drm_device_list[i] = dev;
            break;
        }
    }
    spin_unlock(&drm_device_list_lock);
}

void drm_device_list_remove(struct drm_device *dev)
{
    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        if (drm_device_list[i] == dev) {
            drm_device_list[i] = NULL;
            break;
        }
    }
    spin_unlock(&drm_device_list_lock);
}

struct drm_device *drm_get_singleton(void)
{
    /* Return the first registered primary device for backward
     * compatibility. New code should use drm_get_device_by_minor. */
    spin_lock(&drm_device_list_lock);
    dbg_puts("DRMSING: list[0]=");
    dbg_puts_hex((uint64_t)(uintptr_t)drm_device_list[0]);
    dbg_puts("\r\n");
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        if (drm_device_list[i]) {
            struct drm_device *dev = drm_device_list[i];
            spin_unlock(&drm_device_list_lock);
            return dev;
        }
    }
    spin_unlock(&drm_device_list_lock);
    return NULL;
}

struct drm_device *drm_get_device_by_minor(int type, int index)
{
    spin_lock(&drm_device_list_lock);
    for (int i = 0; i < DRM_MAX_DEVICES; i++) {
        struct drm_device *dev = drm_device_list[i];
        if (!dev) continue;
        if (type == DRM_MINOR_PRIMARY && dev->primary && dev->primary->index == index) {
            spin_unlock(&drm_device_list_lock);
            return dev;
        }
        if (type == DRM_MINOR_RENDER && dev->render && dev->render->index == index) {
            spin_unlock(&drm_device_list_lock);
            return dev;
        }
    }
    spin_unlock(&drm_device_list_lock);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Dummy driver for the built-in DRM node                              */
/* ------------------------------------------------------------------ */

static int drm_dummy_open(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
    return 0;
}

static void drm_dummy_postclose(struct drm_device *dev, struct drm_file *file)
{
    (void)dev;
    (void)file;
}

static void drm_dummy_lastclose(struct drm_device *dev)
{
    (void)dev;
}

static void drm_dummy_gem_free_object(struct drm_gem_object *obj)
{
    if (obj) {
        aligned_free(obj->backing);
        obj->backing = NULL;
        free(obj->dma_buf);
        obj->dma_buf = NULL;
    }
}

static struct drm_gem_object *drm_dummy_gem_prime_import(struct drm_device *dev, void *dma_buf)
{
    /* For the dummy driver, we can only import buffers that were
     * exported by ourselves. The dma_buf pointer is actually a
     * drm_gem_object pointer. */
    struct drm_gem_object *obj = (struct drm_gem_object *)dma_buf;

    (void)dev;
    if (!obj) { return NULL; }
    drm_gem_object_get(obj);
    return obj;
}

static const struct drm_ioctl_desc drm_dummy_ioctls[] = {
    {DRM_IOCTL_VERSION,                drm_version,                      0                    },
    {DRM_IOCTL_GET_MAGIC,              drm_getmagic,                     DRM_AUTH             },
    {DRM_IOCTL_SET_VERSION,            drm_setversion,                   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_SET_MASTER,             drm_setmaster,                    0                    },
    {DRM_IOCTL_DROP_MASTER,            drm_dropmaster,                   0                    },
    {DRM_IOCTL_AUTH_MAGIC,             drm_authmagic,                    DRM_AUTH             },
    {DRM_IOCTL_GEM_CLOSE,              drm_gem_close_ioctl,              DRM_AUTH             },
    {DRM_IOCTL_GEM_FLINK,              drm_gem_flink_ioctl,              DRM_AUTH             },
    {DRM_IOCTL_GEM_OPEN,               drm_gem_open_ioctl,               DRM_AUTH             },
    {DRM_IOCTL_GET_CAP,                drm_get_cap,                      0                    },
    {DRM_IOCTL_SET_CLIENT_CAP,         drm_set_client_cap,               0                    },
    {DRM_IOCTL_WAIT_VBLANK,            drm_wait_vblank_ioctl,            0                    },
    {DRM_IOCTL_MODE_GETRESOURCES,      drm_mode_getresources,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETCRTC,           drm_mode_getcrtc,                 DRM_AUTH             },
    {DRM_IOCTL_MODE_SETCRTC,           drm_mode_setcrtc,                 DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CURSOR,            drm_mode_cursor_ioctl,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETENCODER,        drm_mode_getencoder,              DRM_AUTH             },
    {DRM_IOCTL_MODE_GETCONNECTOR,      drm_mode_getconnector,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETPROPERTY,       drm_mode_getproperty_ioctl,       DRM_AUTH             },
    {DRM_IOCTL_MODE_GETFB,             drm_mode_getfb,                   DRM_AUTH             },
    {DRM_IOCTL_MODE_ADDFB,             drm_mode_addfb,                   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_RMFB,              drm_mode_rmfb,                    DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_PAGE_FLIP,         drm_mode_page_flip_ioctl,         DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_DIRTYFB,           drm_mode_dirtyfb,                 DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_GETPLANERESOURCES, drm_mode_getplane_res,            DRM_AUTH             },
    {DRM_IOCTL_MODE_GETPLANE,          drm_mode_getplane,                DRM_AUTH             },
    {DRM_IOCTL_MODE_SETPLANE,          drm_mode_setplane,                DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_ADDFB2,            drm_mode_addfb2,                  DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_OBJ_GETPROPERTIES, drm_mode_obj_getproperties_ioctl, DRM_AUTH             },
    {DRM_IOCTL_MODE_OBJ_SETPROPERTY,   drm_mode_obj_setproperty_ioctl,   DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_CURSOR2,           drm_mode_cursor2_ioctl,           DRM_AUTH             },
    {DRM_IOCTL_MODE_ATOMIC,            drm_mode_atomic_ioctl,            DRM_MASTER | DRM_AUTH},
    {DRM_IOCTL_MODE_GETFB2,            drm_mode_getfb2_ioctl,            DRM_AUTH             },
};

static struct drm_driver drm_dummy_driver = {
    .name             = "drm",
    .desc             = "Uinxed DRM",
    .date             = "20260722",
    .major            = 1,
    .minor            = 0,
    .patchlevel       = 0,
    .driver_features  = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC | DRIVER_PRIME | DRIVER_RENDER,
    .open             = drm_dummy_open,
    .postclose        = drm_dummy_postclose,
    .lastclose        = drm_dummy_lastclose,
    .gem_free_object  = drm_dummy_gem_free_object,
    .gem_prime_import = drm_dummy_gem_prime_import,
    .dumb_create      = drm_gem_dumb_create,
    .dumb_map_offset  = drm_gem_dumb_map_offset,
    .dumb_destroy     = drm_gem_dumb_destroy,
    .ioctls           = drm_dummy_ioctls,
    .num_ioctls       = sizeof(drm_dummy_ioctls) / sizeof(drm_dummy_ioctls[0]),
};

/* ------------------------------------------------------------------ */
/* KMS pipeline setup for the dummy driver                              */
/* ------------------------------------------------------------------ */

static struct drm_crtc      pipeline_crtc;
static struct drm_plane     pipeline_primary_plane;
static struct drm_encoder   pipeline_encoder;
static struct drm_connector pipeline_connector;

/* ------------------------------------------------------------------ */
/* Configurable mode table - data-driven, not hardcoded in logic       */
/* ------------------------------------------------------------------ */

struct dummy_mode_cfg {
        const char *name;
        int         clock;
        int         hdisplay;
        int         hsync_start;
        int         hsync_end;
        int         htotal;
        int         vdisplay;
        int         vsync_start;
        int         vsync_end;
        int         vtotal;
        int         vrefresh;
        unsigned    flags;
        unsigned    type;
};

/* Scanout bridge: the ported atomic stack owns the KMS object model; the
 * pixels land in the console framebuffer, which is also what text mode
 * draws through -- so a successful page flip visibly replaces the tty. */
#include "../../fbcon.h"

/* The framebuffer currently being scanned out.  The pixman renderer draws
 * in place into the dumb buffer, so after the first commit no ioctl ever
 * changes state again -- without a periodic re-blit the screen would keep
 * showing whatever that first commit contained. */
static struct drm_framebuffer *g_scan_fb;

/* Called from the PIT tick (timer_irq): push the live dumb buffer to the
 * console framebuffer and stamp the cursor over it.  Cheap enough at
 * console resolutions, and it needs no vblank client to be waiting. */
void drm_dummy_refresh(void)
{
    if (!g_scan_fb || !g_scan_fb->obj[0] || !g_scan_fb->obj[0]->backing)
        return;
    uint32_t dw = 0, dh = 0, dpitch = 0;
    fbcon_geometry(&dw, &dh, &dpitch);
    uint32_t *dst = (uint32_t *)fbcon_fb();
    const uint8_t *src = (const uint8_t *)g_scan_fb->obj[0]->backing;
    if (!dst || !src || !dw || !dpitch)
        return;
    uint32_t w = g_scan_fb->width  > dw ? dw : g_scan_fb->width;
    uint32_t h = g_scan_fb->height > dh ? dh : g_scan_fb->height;
    uint32_t dstep = dpitch / 4;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t       *d = dst + (uint64_t)row * dstep;
        const uint32_t *s = (const uint32_t *)(src + (uint64_t)row * g_scan_fb->pitches[0]);
        for (uint32_t col = 0; col < w; col++)
            d[col] = s[col];
    }
    drm_dummy_draw_cursor(dst, dw, dh, dstep);
}

static int drm_dummy_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb,
                               struct drm_pending_vblank_event *event,
                               uint32_t flags)
{
    (void)crtc; (void)event; (void)flags;
    if (!fb)
        return 0;
    if (!fb->obj[0] || !fb->obj[0]->backing)
        return -EINVAL;
    g_scan_fb = fb;

    uint32_t dw = 0, dh = 0, dpitch = 0;
    fbcon_geometry(&dw, &dh, &dpitch);
    uint32_t *dst = (uint32_t *)fbcon_fb();
    const uint8_t *src = (const uint8_t *)fb->obj[0]->backing;
    if (!dst || !src || !dw || !dpitch)
        return -EINVAL;

    /* Clip to the active scanout; the client may have picked a mode larger
     * than the display was booted with. */
    uint32_t w = fb->width  > dw ? dw : fb->width;
    uint32_t h = fb->height > dh ? dh : fb->height;
    uint32_t dstep = dpitch / 4;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t       *d = dst + (uint64_t)row * dstep;
        const uint32_t *s = (const uint32_t *)(src + (uint64_t)row * fb->pitches[0]);
        for (uint32_t col = 0; col < w; col++)
            d[col] = s[col];
    }
    return 0;
}
/* Re-blit the primary framebuffer on every software vblank.  The pixman
 * renderer draws in place into the dumb buffer, so the framebuffer never
 * changes and no page flip ever fires -- without this hook the display
 * would freeze on whatever the first commit happened to contain. */
void drm_dummy_draw_cursor(uint32_t *dst, uint32_t dw, uint32_t dh,
                           uint32_t dstep);
static void drm_dummy_vblank_blit(struct drm_crtc *crtc)
{
    struct drm_framebuffer *fb = NULL;

    if (!crtc || !crtc->primary)
        return;
    if (crtc->primary->state)
        fb = crtc->primary->state->fb;
    if (!fb)
        return;
    if (drm_dummy_page_flip(crtc, fb, NULL, 0) == 0) {
        /* The main blit only runs when the helper succeeds; stamping the
         * cursor here keeps it above whatever was just scanned out. */
        uint32_t dw = 0, dh = 0, dpitch = 0;
        fbcon_geometry(&dw, &dh, &dpitch);
        uint32_t *dst = (uint32_t *)fbcon_fb();
        if (dst && dw && dpitch)
            drm_dummy_draw_cursor(dst, dw, dh, dpitch / 4);
    }
}

/* ---- software cursor ----------------------------------------------------
 * No hardware overlay exists, so the cursor plane is drawn by the vblank
 * blit: cursor_set/keep the ARGB source, and drm_dummy_vblank_blit stamps
 * it over the scanout after the main copy. */
static spinlock_t sw_cursor_lock;

static struct {
    struct drm_gem_object *bo;
    uint32_t               w, h;
    int32_t                hot_x, hot_y;
    int32_t                x, y;      /* top-left of the cursor image */
    bool                   on;
} sw_cursor;

static int drm_dummy_cursor_set(struct drm_crtc *crtc, struct drm_gem_object *bo,
                                uint32_t width, uint32_t height,
                                int32_t hot_x, int32_t hot_y)
{
    (void)crtc;
    spin_lock(&sw_cursor_lock);
    sw_cursor.bo    = bo;
    sw_cursor.w     = width;
    sw_cursor.h     = height;
    sw_cursor.hot_x = hot_x;
    sw_cursor.hot_y = hot_y;
    sw_cursor.on    = (bo != NULL);
    spin_unlock(&sw_cursor_lock);
    return 0;
}

static int drm_dummy_cursor_move(struct drm_crtc *crtc, int32_t x, int32_t y)
{
    (void)crtc;
    spin_lock(&sw_cursor_lock);
    sw_cursor.x = x - sw_cursor.hot_x;
    sw_cursor.y = y - sw_cursor.hot_y;
    spin_unlock(&sw_cursor_lock);
    return 0;
}

/* Stamp the cursor image over `dst` (the console framebuffer).  Pixels with
 * their high byte clear are treated as transparent; everything else is
 * copied verbatim -- cheap, and close enough for an X shape. */
void drm_dummy_draw_cursor(uint32_t *dst, uint32_t dw, uint32_t dh,
                           uint32_t dstep)
{
    const uint32_t *src;
    uint32_t cw, ch;
    int32_t  cx, cy;

    spin_lock(&sw_cursor_lock);
    if (!sw_cursor.on || !sw_cursor.bo || !sw_cursor.bo->backing) {
        spin_unlock(&sw_cursor_lock);
        return;
    }
    src = (const uint32_t *)sw_cursor.bo->backing;
    cw  = sw_cursor.w;
    ch  = sw_cursor.h;
    cx  = sw_cursor.x;
    cy  = sw_cursor.y;
    spin_unlock(&sw_cursor_lock);

    if (cx < 0) { cx = 0; }
    if (cy < 0) { cy = 0; }
    if ((uint32_t)cx >= dw || (uint32_t)cy >= dh)
        return;
    if (cw > dw - (uint32_t)cx) cw = dw - (uint32_t)cx;
    if (ch > dh - (uint32_t)cy) ch = dh - (uint32_t)cy;

    for (uint32_t row = 0; row < ch; row++) {
        uint32_t       *d = dst + (uint64_t)(cy + row) * dstep + cx;
        const uint32_t *srow = src + (uint64_t)row * sw_cursor.w;
        for (uint32_t col = 0; col < cw; col++) {
            uint32_t px = srow[col];
            if ((px >> 24) > 127)
                d[col] = px;
        }
    }
}

static const struct drm_crtc_helper_funcs pipeline_crtc_helper = {
    .page_flip   = drm_dummy_page_flip,
    .vblank      = drm_dummy_vblank_blit,
    .cursor_set  = drm_dummy_cursor_set,
    .cursor_move = drm_dummy_cursor_move,
};


static const struct dummy_mode_cfg dummy_modes[] = {
    {
     .name        = "1920x1080",
     .clock       = 148500,
     .hdisplay    = 1920,
     .hsync_start = 2008,
     .hsync_end   = 2052,
     .htotal      = 2200,
     .vdisplay    = 1080,
     .vsync_start = 1084,
     .vsync_end   = 1089,
     .vtotal      = 1125,
     .vrefresh    = 60,
     .flags       = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
     .type        = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER,
     },
    {
     .name        = "1280x720",
     .clock       = 74250,
     .hdisplay    = 1280,
     .hsync_start = 1390,
     .hsync_end   = 1430,
     .htotal      = 1650,
     .vdisplay    = 720,
     .vsync_start = 725,
     .vsync_end   = 730,
     .vtotal      = 750,
     .vrefresh    = 60,
     .flags       = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
     .type        = DRM_MODE_TYPE_DRIVER,
     },
};

static int drm_dummy_kms_add_modes(struct drm_device *dev, struct drm_connector *connector)
{
    unsigned int i;

    (void)dev;

    for (i = 0; i < sizeof(dummy_modes) / sizeof(dummy_modes[0]); i++) {
        const struct dummy_mode_cfg *cfg = &dummy_modes[i];
        struct drm_display_mode     *mode;

        mode = drm_mode_create(dev);
        if (!mode) { return -ENOMEM; }

        strncpy(mode->name, cfg->name, DRM_DISPLAY_MODE_LEN - 1);
        mode->name[DRM_DISPLAY_MODE_LEN - 1] = '\0';
        mode->clock                          = cfg->clock;
        mode->hdisplay                       = cfg->hdisplay;
        mode->hsync_start                    = cfg->hsync_start;
        mode->hsync_end                      = cfg->hsync_end;
        mode->htotal                         = cfg->htotal;
        mode->vdisplay                       = cfg->vdisplay;
        mode->vsync_start                    = cfg->vsync_start;
        mode->vsync_end                      = cfg->vsync_end;
        mode->vtotal                         = cfg->vtotal;
        mode->vrefresh                       = cfg->vrefresh;
        mode->flags                          = cfg->flags;
        mode->type                           = cfg->type;
        mode->status                         = MODE_OK;

        drm_mode_probed_add(connector, mode);
    }

    return 0;
}

static void drm_refresh_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sched_block_timeout((uint32_t)DRM_WAIT_SLEEP, 1);   /* one PIT tick per frame */
        drm_dummy_refresh();
    }
}

static int drm_dummy_kms_setup(struct drm_device *dev)
{
    static const uint32_t primary_formats[] = {
        DRM_FORMAT_XRGB8888,
        DRM_FORMAT_ARGB8888,
        DRM_FORMAT_RGB888,
        DRM_FORMAT_RGB565,
    };
    int ret;

    memset(&pipeline_crtc, 0, sizeof(pipeline_crtc));
    memset(&pipeline_primary_plane, 0, sizeof(pipeline_primary_plane));
    memset(&pipeline_encoder, 0, sizeof(pipeline_encoder));
    memset(&pipeline_connector, 0, sizeof(pipeline_connector));

    /* Create primary plane (can only be driven by CRTC 0) */
    ret = drm_plane_init(dev, &pipeline_primary_plane, 1, /* possible_crtcs = bit 0 */
                         NULL, primary_formats, sizeof(primary_formats) / sizeof(primary_formats[0]), NULL, DRM_PLANE_TYPE_PRIMARY, "primary");
    if (ret) {
        DRM_ERROR("Failed to init primary plane: %d\n", ret);
        return ret;
    }

    /* Allocate and initialise the primary plane state */
    pipeline_primary_plane.state = malloc(sizeof(*pipeline_primary_plane.state));
    if (!pipeline_primary_plane.state) {
        DRM_ERROR("Failed to alloc primary plane state\n");
        return -ENOMEM;
    }
    memset(pipeline_primary_plane.state, 0, sizeof(*pipeline_primary_plane.state));
    pipeline_primary_plane.state->plane            = &pipeline_primary_plane;
    pipeline_primary_plane.state->crtc             = &pipeline_crtc;
    pipeline_primary_plane.state->rotation         = 0;
    pipeline_primary_plane.state->alpha            = 0xFFFF;
    pipeline_primary_plane.state->pixel_blend_mode = 0;
    pipeline_primary_plane.state->visible          = true;

    /* Create CRTC with the primary plane */
    ret = drm_crtc_init_with_planes(dev, &pipeline_crtc, &pipeline_primary_plane, NULL,
                                    &pipeline_crtc_helper, "CRTC-0");
    if (ret) {
        DRM_ERROR("Failed to init CRTC: %d\n", ret);
        return ret;
    }

    /* Allocate and initialise the CRTC state */
    pipeline_crtc.state = malloc(sizeof(*pipeline_crtc.state));
    if (!pipeline_crtc.state) {
        DRM_ERROR("Failed to alloc CRTC state\n");
        return -ENOMEM;
    }
    memset(pipeline_crtc.state, 0, sizeof(*pipeline_crtc.state));
    pipeline_crtc.state->crtc   = &pipeline_crtc;
    pipeline_crtc.state->active = false;
    pipeline_crtc.state->enable = false;

    /* Create encoder (VIRTUAL type for software-only output) */
    ret = drm_encoder_init(dev, &pipeline_encoder, NULL, DRM_MODE_ENCODER_VIRTUAL, "encoder-0");
    if (ret) {
        DRM_ERROR("Failed to init encoder: %d\n", ret);
        return ret;
    }
    pipeline_encoder.possible_crtcs = 1;
    pipeline_encoder.crtc           = &pipeline_crtc;

    /* Create connector (VIRTUAL, initially connected) */
    ret = drm_connector_init(dev, &pipeline_connector, NULL, DRM_MODE_CONNECTOR_VIRTUAL);
    if (ret) {
        DRM_ERROR("Failed to init connector: %d\n", ret);
        return ret;
    }
    pipeline_connector.status                 = connector_status_connected;
    pipeline_connector.display_info_width_mm  = 500;
    pipeline_connector.display_info_height_mm = 280;

    /* Allocate and initialise the connector state */
    pipeline_connector.state = malloc(sizeof(*pipeline_connector.state));
    if (!pipeline_connector.state) {
        DRM_ERROR("Failed to alloc connector state\n");
        return -ENOMEM;
    }
    memset(pipeline_connector.state, 0, sizeof(*pipeline_connector.state));
    pipeline_connector.state->connector    = &pipeline_connector;
    pipeline_connector.state->crtc         = &pipeline_crtc;
    pipeline_connector.state->best_encoder = &pipeline_encoder;

    /* Attach encoder to connector */
    ret = drm_connector_attach_encoder(&pipeline_connector, &pipeline_encoder);
    if (ret) {
        DRM_ERROR("Failed to attach encoder: %d\n", ret);
        return ret;
    }

    /* Add display modes from the configurable table */
    ret = drm_dummy_kms_add_modes(dev, &pipeline_connector);
    if (ret) {
        DRM_ERROR("Failed to add modes: %d\n", ret);
        return ret;
    }

    /* Initialise vblank for this CRTC */
    ret = drm_vblank_init(dev, 1);
    if (ret) {
        DRM_ERROR("Failed to init vblank: %d\n", ret);
        return ret;
    }

    drm_connector_register(&pipeline_connector);

    DRM_INFO("KMS pipeline: CRTC-%u + primary plane-%u + encoder-%u + connector-%u (%u modes)\n", pipeline_crtc.base.id,
             pipeline_primary_plane.base.id, pipeline_encoder.base.id, pipeline_connector.base.id, sizeof(dummy_modes) / sizeof(dummy_modes[0]));

    /* Periodic scanout refresh.  The pixman renderer draws in place into
     * the dumb buffer and the framebuffer never changes after the first
     * commit, so without a re-blit the display would freeze on that first
     * frame.  This used to hang off the PIT tick, but a multi-megabyte
     * copy inside the timer interrupt starved the whole machine; as a
     * kernel thread it just competes for CPU like everything else. */
    if (!kthread_create("drm-refresh", drm_refresh_thread, NULL))
        return -ENOMEM;

    return 0;
}


/* ------------------------------------------------------------------ */
/* DRM VFS ioctl wrapper                                               */
/* ------------------------------------------------------------------ */

size_t drm_dev_read(void *file, void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return 0;
}

size_t drm_dev_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return 0;
}

int drm_dev_ioctl(void *file, size_t req, void *arg)
{
    struct drm_device *dev;
    struct drm_file   *file_priv = (struct drm_file *)file;

    if (!file_priv) return -ENODEV;

    dev = drm_get_singleton();
    if (!dev) return -ENODEV;

    return drm_ioctl(dev, (unsigned int)req, arg, file_priv);
}

/* tmpfs/devtmpfs per-open bridge. A VFS node is shared by all processes, so
 * storing drm_file in node->handle is incorrect: one close could release
 * another client's state. */
int drm_dev_open(void *node_ptr, uint64_t flags, void **private_data)
{
    struct drm_device *dev;
    struct drm_file   *file;
    int                ret;

    (void)node_ptr;
    (void)flags;
    if (!private_data) return -EINVAL;
    *private_data = NULL;

    dev = drm_get_singleton();
    if (!dev) return -ENODEV;
    file = malloc(sizeof(*file));
    if (!file) return -ENOMEM;
    memset(file, 0, sizeof(*file));
    ret = drm_open(dev, file);
    if (ret) {
        free(file);
        return ret;
    }

    /* A root compositor opening the primary node is already trusted for
     * DRM_AUTH ioctls.  Weston performs GETRESOURCES immediately after the
     * open (before issuing SET_MASTER); leaving this bit clear makes the
     * otherwise valid KMS device look absent to its DRM backend.  Render
     * nodes intentionally keep the normal unauthenticated state. */
    struct vfs_node *node = (struct vfs_node *)node_ptr;
    process_t       *proc = process_current();
    if (node && node->name[0] && !strncmp(node->name, "card", 4) && proc && proc->uid == 0) file->authenticated = true;
    *private_data = file;
    return 0;
}

void drm_dev_release(void *node_ptr, void *private_data)
{
    (void)node_ptr;
    if (private_data) {
        drm_release((struct drm_file *)private_data);
        free(private_data);
    }
}

int drm_dev_file_ioctl(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg)
{
    (void)ctx;
    (void)flags;
    return drm_dev_ioctl(private_data, req, arg);
}

int64_t drm_dev_file_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)flags;
    return (int64_t)drm_dev_read(private_data, addr, offset, size);
}

int64_t drm_dev_file_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)flags;
    return (int64_t)drm_dev_write(private_data, addr, offset, size);
}

int drm_dev_file_poll(void *ctx, void *private_data, uint64_t flags, size_t events)
{
    (void)ctx;
    (void)flags;
    return drm_dev_poll(private_data, events);
}

int drm_dev_poll(void *file, size_t events)
{
    (void)file;
    (void)events;
    return 0;
}

void *drm_dev_mmap(void *file, size_t offset, size_t size, int flags)
{
    struct drm_device     *dev;
    struct drm_file       *file_priv = (struct drm_file *)file;
    struct drm_gem_object *obj;
    void                  *result;

    (void)flags;
    (void)size;

    if (!file_priv) return NULL;

    dev = drm_get_singleton();
    if (!dev) return NULL;

    /* Look up the GEM object by the mmap offset that was returned
     * from MAP_DUMB. Its backing memory is identity-mapped (physical
     * == virtual) so we can return the pointer directly. */
    obj = drm_gem_object_lookup_by_offset(file_priv, (uint64_t)offset);
    if (!obj) { return NULL; }

    result = obj->backing;
    drm_gem_object_put(obj);
    return result;
}

/* ------------------------------------------------------------------ */
/* DRM per-open mmap callback (VMA-aware GEM mmap)                     */
/* ------------------------------------------------------------------ */

void *drm_dev_file_mmap(void *ctx, void *private_data, uint64_t offset, uint64_t size, int flags, struct vm_area *vma)
{
    struct drm_device     *dev       = (struct drm_device *)ctx;
    struct drm_file       *file_priv = (struct drm_file *)private_data;
    struct drm_gem_object *obj;

    (void)size;
    (void)flags;

    if (!dev) dev = drm_get_singleton();
    dbg_puts("DRMMAP: dev=");
    dbg_puts_hex((uint64_t)(uintptr_t)dev);
    dbg_puts(" fp=");
    dbg_puts_hex((uint64_t)(uintptr_t)file_priv);
    dbg_puts(" off=");
    dbg_puts_hex(offset);
    dbg_puts(" sz=");
    dbg_puts_hex((uint64_t)size);
    dbg_puts(" fl=");
    dbg_puts_hex((uint64_t)flags);
    dbg_puts(" vma=");
    dbg_puts_hex((uint64_t)(uintptr_t)vma);
    dbg_puts("\r\n");
    if (!dev || !file_priv || !vma) return NULL;

    /* Look up the GEM object by its mmap offset. */
    obj = drm_gem_object_lookup_by_offset(file_priv, (uint64_t)offset);
    dbg_puts("DRMMAP: obj=");
    dbg_puts_hex((uint64_t)(uintptr_t)obj);
    dbg_puts(" backing=");
    dbg_puts_hex(obj ? (uint64_t)(uintptr_t)obj->backing : 0);
    dbg_puts("\r\n");
    if (!obj || !obj->backing) return NULL;

    /* Store GEM object in VMA for lifetime tracking.
     * process_munmap will call drm_gem_object_put when the
     * mapping is torn down. */
    vma->vm_private_data = obj;

    /* Identity-mapped physical memory: return the backing pointer.
     * The syscall mmap layer handles PTE creation using this pointer. */
    return obj->backing;
}

/* ------------------------------------------------------------------ */
/* DRM open / release callbacks for devtmpfs                           */
/* ------------------------------------------------------------------ */

/*
 * When userspace opens /dev/dri/card0, tmpfs calls this open callback.
 * GNOS's VFS has no per-open callbacks (the DRM open path runs through
 * drm_dev_open above instead), so these two are inert here.
 */
void drm_vfs_open_cb(void *parent, const char *name, void *node_ptr)
{
    (void)parent;
    (void)name;
    (void)node_ptr;
}

void drm_vfs_close_cb(void *current)
{
    (void)current;
}

/* ------------------------------------------------------------------ */
/* DRM class (global, shared by all DRM devices)                       */
/* ------------------------------------------------------------------ */

static int drm_device_uevent(struct device *dev, struct kobj_uevent_env *env)
{
    (void)dev;
    /* Match the Linux DRM minor contract consumed by eudev/libudev. */
    return add_uevent_var(env, "DEVTYPE=drm_minor");
}

struct class drm_class = {
    .name      = "drm",
    .dev_uevent = drm_device_uevent,
};
int drm_class_registered = 0;

/* ------------------------------------------------------------------ */
/* Public init                                                         */
/* ------------------------------------------------------------------ */

int drm_init(void)
{
#if CONFIG_DRM
    /* Register core DRM services first.  The software fallback must not
     * claim card0/renderD128 before a hardware driver probes. */
    if (!drm_class_registered) {
        int ret = class_register(&drm_class);
        if (ret != EOK) return ret;
        drm_class_registered = 1;
    }
    return 0;
#else
    return -ENODEV;
#endif
}

int drm_init_fallback(void)
{
#if CONFIG_DRM
    struct drm_device *dev;

    dev = drm_dev_alloc(&drm_dummy_driver);
    if (!dev) {
        DRM_ERROR("Failed to allocate DRM device\n");
        return -ENOMEM;
    }

    int ret = drm_dev_register(dev, 0);
    if (ret != 0) {
        DRM_ERROR("Failed to register DRM device: %d\n", ret);
        free(dev);
        return ret;
    }

    /* Set up the minimal KMS display pipeline */
    ret = drm_dummy_kms_setup(dev);
    if (ret != 0) {
        DRM_ERROR("Failed to set up KMS pipeline: %d\n", ret);
        free(dev);
        return ret;
    }

    drm_device_list_add(dev);
    dbg_puts("DRMINIT: fallback device=");
    dbg_puts_hex((uint64_t)(uintptr_t)dev);
    dbg_puts(" list[0]=");
    dbg_puts_hex((uint64_t)(uintptr_t)drm_device_list[0]);
    dbg_puts("\r\n");

    return 0;
#else
    return -ENODEV;
#endif
}
