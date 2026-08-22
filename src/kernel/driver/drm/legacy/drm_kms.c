/*
 * drm_kms.c — KMS object queries and modesetting: resources, connector,
 * encoder, crtc (GET/SETCRTC), plane resources and the page flip. (GPLv2)
 *
 * One CRTC, one connector, one primary plane, all object id 1.  wlroots
 * refuses to start without plane resources, so the plane family is a hard
 * requirement for the labwc line.
 */
#include "drm_internal.h"
#include "fbcon.h"
#include "debugcon.h"
#include "kstring.h"

int32_t drm_ioctl_get_resources(uint64_t arg)
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

int32_t drm_ioctl_get_connector(uint64_t arg)
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
    if (c.props_ptr && c.count_props >= 1) {
        static const uint32_t props[1] = { DRM_PROP_ID_DPMS };
        copy_to_user((uint64_t)(uintptr_t)c.props_ptr, props, sizeof props);
    }
    if (c.prop_values_ptr && c.count_props >= 1) {
        static const uint64_t vals[1] = { 0 };   /* DPMS On */
        copy_to_user((uint64_t)(uintptr_t)c.prop_values_ptr, vals,
                     sizeof vals);
    }

    c.count_modes = N_MODES;
    c.count_props = 1;
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

int32_t drm_ioctl_get_encoder(uint64_t arg)
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

int32_t drm_ioctl_get_crtc(uint64_t arg)
{
    drm_mode_crtc_t c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -E_FAULT;
    dbg_puts("SETCRTC id=");
    dbg_puts_dec((uint32_t)c.crtc_id);
    dbg_puts(" fb=");
    dbg_puts_dec((uint32_t)c.fb_id);
    dbg_puts(" nconn=");
    dbg_puts_dec((uint32_t)c.count_connectors);
    dbg_puts(" mv=");
    dbg_puts_dec((uint32_t)c.mode_valid);
    dbg_puts("\n");
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

int32_t drm_ioctl_set_crtc(uint64_t arg)
{
    drm_mode_crtc_t c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -E_FAULT;
    dbg_puts("SETCRTC id=");
    dbg_puts_dec((uint32_t)c.crtc_id);
    dbg_puts(" fb=");
    dbg_puts_dec((uint32_t)c.fb_id);
    dbg_puts(" nconn=");
    dbg_puts_dec((uint32_t)c.count_connectors);
    dbg_puts(" mv=");
    dbg_puts_dec((uint32_t)c.mode_valid);
    dbg_puts("\n");
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
        if (!f) {
            dbg_puts("SETCRTC: fb_by_id miss\n");
            return -E_NOENT;
        }
        d = dumb_by_handle(f->handle);
        if (!d) {
            dbg_puts("SETCRTC: dumb_by_handle miss\n");
            return -E_NOENT;
        }
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

/* One primary plane, pinned to crtc 1.  wlroots refuses to start without
 * plane resources, so this is a hard requirement for the labwc line. */
int32_t drm_ioctl_get_plane_res(uint64_t arg)
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

int32_t drm_ioctl_get_plane(uint64_t arg)
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

int32_t drm_ioctl_page_flip(uint64_t arg)
{
    struct {
        uint32_t crtc_id, fb_id, flags, reserved;
        uint64_t user_data;
    } p;
    if (copy_from_user(&p, arg, sizeof p) < 0)
        return -E_FAULT;
    if (p.crtc_id != 1)
        return -E_NOENT;
    if (p.flags & ~DRM_MODE_PAGE_FLIP_EVENT)
        return -E_INVAL;                /* no async; the event flag is served */
    fb_t *f = fb_by_id(p.fb_id);
    if (!f)
        return -E_NOENT;
    dumb_t *d = dumb_by_handle(f->handle);
    if (!d)
        return -E_NOENT;
    /* No vblank here: a page flip is an immediate blit. */
    blit_fb(d->phys, d->pitch, f->width, f->height, 0, 0);
    g_cur_fb = f->fb_id;

    /* The flip is done; satisfy the client that asked for an event. */
    if (p.flags & DRM_MODE_PAGE_FLIP_EVENT)
        drm_queue_event(p.user_data);

    return 0;
}
