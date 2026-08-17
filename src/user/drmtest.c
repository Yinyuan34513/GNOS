/*
 * drmtest.c — /dev/dri/card0 self-test. (GPLv2, musl)
 *
 * Boot-time proof that the kernel's DRM/KMS driver speaks the Linux UAPI:
 * version/caps, resources, the connector's mode list, a dumb buffer
 * (create/map/mmap/fill), an fb, and two SETCRTCs -- a real mode switch
 * through the bochs VBE registers and back to the boot mode, each followed
 * by a blit of a test pattern.  Every verdict goes to the debug console via
 * dbgputs(441) so `make test` can read it headlessly; the pattern itself
 * is visible on the framebuffer for the two seconds each mode holds.
 */
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <stdarg.h>

/* ---- the UAPI subset, byte-copies from the kernel's drm.h -------------- */
typedef struct {
    int version_major, version_minor, version_patchlevel;
    unsigned long name_len;  void *name;
    unsigned long date_len;  void *date;
    unsigned long desc_len;  void *desc;
} drm_version_t;

typedef struct { unsigned long capability, value; } drm_get_cap_t;

typedef struct {
    unsigned long fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    unsigned count_fbs, count_crtcs, count_connectors, count_encoders;
    unsigned min_width, max_width, min_height, max_height;
} drm_mode_card_res_t;

typedef struct {
    unsigned clock;
    unsigned short hdisplay, hsync_start, hsync_end, htotal, hskew;
    unsigned short vdisplay, vsync_start, vsync_end, vtotal, vscan;
    unsigned vrefresh, flags, type;
    char name[32];
} drm_mode_modeinfo_t;

typedef struct {
    unsigned long encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    unsigned count_modes, count_props, count_encoders;
    unsigned encoder_id, connector_id, connector_type, connector_type_id;
    unsigned connection, mm_width, mm_height, subpixel, pad;
} drm_mode_get_connector_t;

typedef struct {
    unsigned long set_connectors_ptr;
    unsigned count_connectors;
    unsigned crtc_id, fb_id, x, y, gamma_size, mode_valid;
    drm_mode_modeinfo_t mode;
} drm_mode_crtc_t;

typedef struct {
    unsigned height, width, bpp, flags;
    unsigned handle, pitch;
    unsigned long size;
} drm_mode_create_dumb_t;

typedef struct { unsigned handle, pad; unsigned long offset; } drm_mode_map_dumb_t;
typedef struct { unsigned handle; } drm_mode_destroy_dumb_t;
typedef struct {
    unsigned fb_id, width, height, pitch, bpp, depth, handle;
} drm_mode_fb_cmd_t;
typedef struct {
    unsigned fb_id, width, height, pixel_format, flags;
    unsigned handles[4], pitches[4], offsets[4];
    unsigned long modifier[4];
} drm_mode_fb_cmd2_t;

typedef struct {
    unsigned long plane_id_ptr;
    unsigned count_planes;
} drm_mode_get_plane_res_t;

typedef struct {
    unsigned plane_id, crtc_id, fb_id, possible_crtcs, gamma_size;
    unsigned count_format_types;
    unsigned long format_type_ptr;
} drm_mode_get_plane_t;

typedef struct {
    unsigned long value;
    char name[32];
} drm_mode_property_enum_t;

typedef struct {
    unsigned long values_ptr, enum_blob_ptr;
    unsigned prop_id, flags;
    char name[32];
    unsigned count_values, count_enum_blobs;
} drm_mode_get_property_t;

typedef struct {
    unsigned long props_ptr, prop_values_ptr;
    unsigned count_props, obj_id, obj_type;
} drm_mode_obj_get_properties_t;

#define DRM_IOCTL_VERSION       0xc0406400
#define DRM_IOCTL_GET_CAP       0xc010640c
#define DRM_IOCTL_SET_CLIENT_CAP 0x4010640d
#define DRM_IOCTL_MODE_GETRESOURCES 0xc04064a0
#define DRM_IOCTL_MODE_GETCONNECTOR 0xc05064a7
#define DRM_IOCTL_MODE_SETCRTC  0xc06864a2
#define DRM_IOCTL_MODE_CREATE_DUMB 0xc02064b2
#define DRM_IOCTL_MODE_MAP_DUMB    0xc01064b3
#define DRM_IOCTL_MODE_DESTROY_DUMB 0xc00464b4
#define DRM_IOCTL_MODE_ADDFB2   0xc06864b8
#define DRM_IOCTL_MODE_RMFB     0xc00464af
#define DRM_IOCTL_MODE_PAGE_FLIP 0xc01864b0
#define DRM_IOCTL_MODE_GETPROPERTY 0xc04064aa
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES 0xc02064b9
#define DRM_IOCTL_MODE_GETPLANERESOURCES 0xc01064b5
#define DRM_IOCTL_MODE_GETPLANE   0xc02864b6

#define DRM_CAP_DUMB_BUFFER 1
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_ARGB8888 0x34324152u
#define DRM_FORMAT_MOD_LINEAR 0
#define DRM_CLIENT_CAP_ATOMIC 3
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_MODE_FB_MODIFIERS (1 << 1)
#define DRM_MODE_PROP_ENUM (1 << 3)
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0u
#define DRM_MODE_OBJECT_PLANE 0xeeeeeeeeu

static int g_failed;
static void report(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
    syscall(441, buf);
}

static void check(int ok, int n, const char *what)
{
    report("DRMTEST: %s %d (%s)", ok ? "PASS" : "FAIL", n, what);
    if (!ok && !g_failed)
        g_failed = n;
}

/* Fill a scanline-aligned 32-bpp buffer with vertical colour bars. */
static void draw_bars(unsigned *pix, unsigned w, unsigned h, unsigned pitch)
{
    for (unsigned y = 0; y < h; y++) {
        unsigned *row = (unsigned *)((char *)pix + (size_t)y * pitch);
        for (unsigned x = 0; x < w; x++) {
            unsigned band = x / (w / 4);
            unsigned c = 0;
            if (band == 0) c = 0x00FF0000;          /* red   */
            else if (band == 1) c = 0x0000FF00;     /* green */
            else if (band == 2) c = 0x000000FF;     /* blue  */
            else c = 0x00FFFFFF;                    /* white */
            row[x] = c;
        }
    }
}

int main(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    check(fd >= 0, 1, "open /dev/dri/card0");
    if (fd < 0)
        return 1;

    /* 1. version */
    {
        drm_version_t v;
        char name[32];
        memset(&v, 0, sizeof v);
        v.name = name; v.name_len = sizeof name;
        int ok = ioctl(fd, DRM_IOCTL_VERSION, &v) == 0 &&
                 v.version_major == 1 && strstr(name, "gnos") != NULL;
        check(ok, 2, "DRM_IOCTL_VERSION");
    }

    /* 2. dumb-buffer capability */
    {
        drm_get_cap_t c = { DRM_CAP_DUMB_BUFFER, 0 };
        check(ioctl(fd, DRM_IOCTL_GET_CAP, &c) == 0 && c.value == 1, 3,
              "DRM_CAP_DUMB_BUFFER");
    }

    /* 3. resources: one crtc, one connector, one encoder */
    {
        drm_mode_card_res_t r;
        memset(&r, 0, sizeof r);
        int ok = ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) == 0 &&
                 r.count_crtcs == 1 && r.count_connectors == 1 &&
                 r.count_encoders == 1;
        check(ok, 4, "MODE_GETRESOURCES");
    }

    /* 4. the connector is connected and offers the four modes */
    {
        drm_mode_get_connector_t c;
        drm_mode_modeinfo_t modes[8];
        memset(&c, 0, sizeof c);
        c.connector_id = 1;
        c.modes_ptr = (unsigned long)modes;
        c.count_modes = 8;
        int ok = ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) == 0 &&
                 c.connection == 0 && c.count_modes >= 4;
        int has_boot = 0;
        for (unsigned i = 0; i < c.count_modes && i < 8; i++)
            if (strcmp(modes[i].name, "1280x800") == 0)
                has_boot = 1;
        check(ok && has_boot, 5, "MODE_GETCONNECTOR");
    }

    /* 5. a dumb buffer we can mmap and fill */
    drm_mode_create_dumb_t cr;
    memset(&cr, 0, sizeof cr);
    cr.width = 1280; cr.height = 800; cr.bpp = 32;
    int ok = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr) == 0 &&
             cr.pitch == 1280 * 4 && cr.size == (unsigned long)1280 * 4 * 800;
    check(ok, 6, "MODE_CREATE_DUMB");

    drm_mode_map_dumb_t mp = { .handle = cr.handle };
    ok = ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mp) == 0 &&
         mp.offset == (unsigned long)cr.handle << 12;
    check(ok, 7, "MODE_MAP_DUMB");

    unsigned *pix = MAP_FAILED;
    if (ok) {
        pix = mmap(NULL, cr.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                   (off_t)mp.offset);
        check(pix != MAP_FAILED, 8, "mmap of the dumb buffer");
    }
    if (pix != MAP_FAILED)
        draw_bars(pix, 1280, 800, cr.pitch);

    /* 6. an fb wrapping the buffer, and a modeset to 1024x768 + blit */
    drm_mode_fb_cmd2_t fb;
    memset(&fb, 0, sizeof fb);
    fb.width = 1280; fb.height = 800;
    fb.pixel_format = DRM_FORMAT_XRGB8888;
    fb.handles[0] = cr.handle;
    fb.pitches[0] = cr.pitch;
    ok = ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) == 0 && fb.fb_id != 0;
    check(ok, 9, "MODE_ADDFB2");

    if (ok) {
        drm_mode_crtc_t set;
        memset(&set, 0, sizeof set);
        set.crtc_id = 1;
        set.mode_valid = 1;
        set.fb_id = fb.fb_id;
        strcpy(set.mode.name, "1024x768");
        set.mode.hdisplay = 1024; set.mode.vdisplay = 768;
        ok = ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) == 0;
        check(ok, 10, "MODE_SETCRTC 1024x768");
        if (ok)
            sleep(2);

        /* and back to the boot mode */
        strcpy(set.mode.name, "1280x800");
        set.mode.hdisplay = 1280; set.mode.vdisplay = 800;
        ok = ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) == 0;
        check(ok, 11, "MODE_SETCRTC back to 1280x800");
        if (ok)
            sleep(2);

        /* the fb blit must have happened: sample a pixel in the middle of
         * the framebuffer's red bar -- (0,0) would be overwritten by the
         * console's own text at the top-left. */
        unsigned *screen = MAP_FAILED;
        int fb0 = open("/dev/fb0", O_RDWR);
        if (fb0 >= 0) {
            screen = mmap(NULL, 1280 * 4 * 800, PROT_READ, MAP_SHARED, fb0, 0);
            check(screen != MAP_FAILED && screen[100 * 1280 + 100] == 0x00FF0000,
                  12, "blit reached the scanout");
            if (screen != MAP_FAILED)
                munmap(screen, 1280 * 4 * 800);
            close(fb0);
        }

        ok = ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb.fb_id) == 0;
        check(ok, 13, "MODE_RMFB");
    }

    if (pix != MAP_FAILED)
        munmap(pix, cr.size);
    drm_mode_destroy_dumb_t dd = { .handle = cr.handle };
    check(ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd) == 0, 14,
          "MODE_DESTROY_DUMB");

    /* ---- the plane/property surface wlroots walks on startup ---- */

    /* 7. client caps: atomic must be refused so wlroots stays on the legacy
     * modeset path; universal planes must be accepted. */
    {
        unsigned long cap[2] = { DRM_CLIENT_CAP_ATOMIC, 1 };
        errno = 0;
        check(ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, cap) < 0 &&
              errno == ENOTSUP, 15, "SET_CLIENT_CAP(ATOMIC) refused");
        cap[0] = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
        check(ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, cap) == 0, 16,
              "SET_CLIENT_CAP(UNIVERSAL_PLANES)");
    }

    /* 8. one primary plane on crtc 1 */
    {
        drm_mode_get_plane_res_t r;
        unsigned ids[4];
        memset(&r, 0, sizeof r);
        r.plane_id_ptr = (unsigned long)ids;
        r.count_planes = 4;
        int ok = ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &r) == 0 &&
                 r.count_planes == 1 && ids[0] == 1;
        check(ok, 17, "MODE_GETPLANERESOURCES");

        drm_mode_get_plane_t p;
        unsigned fmts[4];
        memset(&p, 0, sizeof p);
        p.plane_id = 1;
        p.format_type_ptr = (unsigned long)fmts;
        p.count_format_types = 4;
        ok = ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p) == 0 &&
             p.crtc_id == 1 && p.possible_crtcs == 1 &&
             p.count_format_types >= 2 &&
             (fmts[0] == DRM_FORMAT_XRGB8888 ||
              fmts[1] == DRM_FORMAT_XRGB8888);
        check(ok, 18, "MODE_GETPLANE");
    }

    /* 9. the DPMS and type properties, names and enums */
    {
        drm_mode_get_property_t pr;
        unsigned long vals[8];
        drm_mode_property_enum_t enums[8];

        memset(&pr, 0, sizeof pr);
        pr.prop_id = 1;
        pr.values_ptr = (unsigned long)vals;
        pr.enum_blob_ptr = (unsigned long)enums;
        pr.count_values = 8;
        pr.count_enum_blobs = 8;
        int ok = ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &pr) == 0 &&
                 strcmp(pr.name, "DPMS") == 0 &&
                 (pr.flags & DRM_MODE_PROP_ENUM) &&
                 pr.count_values == 4 && pr.count_enum_blobs == 4 &&
                 strcmp(enums[3].name, "Off") == 0 && enums[3].value == 3;
        check(ok, 19, "GETPROPERTY(DPMS)");

        memset(&pr, 0, sizeof pr);
        pr.prop_id = 3;
        pr.values_ptr = (unsigned long)vals;
        pr.enum_blob_ptr = (unsigned long)enums;
        pr.count_values = 8;
        pr.count_enum_blobs = 8;
        ok = ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &pr) == 0 &&
             strcmp(pr.name, "type") == 0 &&
             strcmp(enums[1].name, "Primary") == 0 && enums[1].value == 1;
        check(ok, 20, "GETPROPERTY(type)");
    }

    /* 10. object properties: connector carries DPMS+EDID, the plane "type" */
    {
        drm_mode_obj_get_properties_t o;
        unsigned pids[8];
        unsigned long pvals[8];
        memset(&o, 0, sizeof o);
        o.props_ptr = (unsigned long)pids;
        o.prop_values_ptr = (unsigned long)pvals;
        o.count_props = 8;
        o.obj_id = 1;
        o.obj_type = DRM_MODE_OBJECT_CONNECTOR;
        int ok = ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &o) == 0 &&
                 o.count_props == 2 && pids[0] == 1 && pids[1] == 2;
        check(ok, 21, "OBJ_GETPROPERTIES(connector)");

        memset(&o, 0, sizeof o);
        o.props_ptr = (unsigned long)pids;
        o.prop_values_ptr = (unsigned long)pvals;
        o.count_props = 8;
        o.obj_id = 1;
        o.obj_type = DRM_MODE_OBJECT_PLANE;
        ok = ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &o) == 0 &&
             o.count_props == 1 && pids[0] == 3 && pvals[0] == 1;
        check(ok, 22, "OBJ_GETPROPERTIES(plane)");
    }

    /* 11. ADDFB2 modifiers: linear is fine, anything else is refused */
    {
        drm_mode_create_dumb_t cr2;
        memset(&cr2, 0, sizeof cr2);
        cr2.width = 64; cr2.height = 64; cr2.bpp = 32;
        ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr2);

        drm_mode_fb_cmd2_t fb2;
        memset(&fb2, 0, sizeof fb2);
        fb2.width = 64; fb2.height = 64;
        fb2.pixel_format = DRM_FORMAT_XRGB8888;
        fb2.handles[0] = cr2.handle;
        fb2.pitches[0] = cr2.pitch;
        fb2.flags = DRM_MODE_FB_MODIFIERS;
        fb2.modifier[0] = DRM_FORMAT_MOD_LINEAR;
        int ok = ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb2) == 0 &&
                 fb2.fb_id != 0;
        check(ok, 23, "ADDFB2 with MOD_LINEAR");
        if (ok) {
            unsigned id = fb2.fb_id;
            ioctl(fd, DRM_IOCTL_MODE_RMFB, &id);
        }

        memset(&fb2, 0, sizeof fb2);
        fb2.width = 64; fb2.height = 64;
        fb2.pixel_format = DRM_FORMAT_XRGB8888;
        fb2.handles[0] = cr2.handle;
        fb2.pitches[0] = cr2.pitch;
        fb2.flags = DRM_MODE_FB_MODIFIERS;
        fb2.modifier[0] = 1;            /* not linear */
        errno = 0;
        check(ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb2) < 0 &&
              errno == ENOTSUP, 24, "ADDFB2 tiled modifier refused");

        drm_mode_destroy_dumb_t dd2 = { .handle = cr2.handle };
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd2);
    }

    close(fd);
    report("DRMTEST: done (%s)", g_failed ? "FAILED" : "ALL PASS");
    return g_failed;
}
