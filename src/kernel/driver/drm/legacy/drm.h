/*
 * drm.h — the DRM/KMS UAPI subset GNOS implements. (GPLv2)
 *
 * These structs and ioctl numbers are byte-for-byte copies of the Linux UAPI
 * (<drm/drm.h>, <drm/drm_mode.h> as shipped by libdrm-dev, x86-64).  Users
 * compute field offsets from their own copies of these headers, so a field
 * dropped or reordered here is not a missing feature but silent corruption.
 * The ioctl numbers are written as literals for the same reason -- they are
 * derived from sizeof() of these very structs, and a typo in a _IOWR would
 * talk to a different ioctl on the user's side.
 *
 * This is the *pre-atomic* connector layout (drm_mode_get_connector without
 * the props machinery) and the pre-modifier fb_cmd2; both match the headers
 * the fastfetch port was built against.
 */
#ifndef GNOS_DRM_H
#define GNOS_DRM_H

#include <stdint.h>
#include <stddef.h>

/* ---- drm.h -------------------------------------------------------------- */

typedef struct {
    int      version_major;
    int      version_minor;
    int      version_patchlevel;
    uint64_t name_len;
    void    *name;
    uint64_t date_len;
    void    *date;
    uint64_t desc_len;
    void    *desc;
} drm_version_t;                        /* 64 bytes */

typedef struct {
    uint64_t capability;
    uint64_t value;
} drm_get_cap_t;                        /* 16 bytes */

typedef struct {
    uint64_t capability;
    uint64_t value;
} drm_set_client_cap_t;                 /* 16 bytes */

typedef struct {
    uint32_t magic;
} drm_auth_t;                           /* 4 bytes */

/* drm.h: PRIME handle <-> dmabuf fd conversion */
typedef struct {
    uint32_t handle;
    uint32_t flags;                 /* DRM_CLOEXEC for handle->fd */
    int32_t  fd;                    /* returned dmabuf fd */
} drm_prime_handle_t;               /* 12 bytes */

/* drm.h: GEM handle close */
typedef struct {
    uint32_t handle;
    uint32_t pad;
} drm_gem_close_t;                  /* 8 bytes */

typedef struct {
    uint64_t unique_len;
    void    *unique;
} drm_unique_t;                         /* 16 bytes */

#define DRM_IOCTL_VERSION       0xc0406400
#define DRM_IOCTL_GET_UNIQUE    0xc0106401
#define DRM_IOCTL_GET_MAGIC     0x80046402
#define DRM_IOCTL_GET_CAP       0xc010640c
#define DRM_IOCTL_SET_CLIENT_CAP 0x4010640d
#define DRM_IOCTL_AUTH_MAGIC    0x40046411
#define DRM_IOCTL_SET_MASTER    0x4000641e
#define DRM_IOCTL_DROP_MASTER   0x4000641f
#define DRM_IOCTL_PRIME_HANDLE_TO_FD 0xc00c642d
#define DRM_IOCTL_PRIME_FD_TO_HANDLE 0xc00c642e
#define DRM_IOCTL_GEM_CLOSE     0x40086409

#define DRM_CAP_DUMB_BUFFER             1
#define DRM_CAP_DUMB_PREFERRED_DEPTH    3
#define DRM_CAP_DUMB_PREFER_SHADOW      4

/* drm.h: PRIME flags (same bit as O_CLOEXEC) */
#define DRM_CLOEXEC  0x80000

/* ---- drm_mode.h ---------------------------------------------------------- */

typedef struct {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width, max_width;
    uint32_t min_height, max_height;
} drm_mode_card_res_t;                  /* 64 bytes */

typedef struct {
    uint32_t clock;                     /* kHz */
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char     name[32];
} drm_mode_modeinfo_t;                  /* 68 bytes */

typedef struct {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width, mm_height;
    uint32_t subpixel;
    uint32_t pad;
} drm_mode_get_connector_t;             /* 80 bytes */

typedef struct {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
} drm_mode_get_encoder_t;               /* 20 bytes */

typedef struct {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x, y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    drm_mode_modeinfo_t mode;
} drm_mode_crtc_t;                      /* 104 bytes */

typedef struct {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
} drm_mode_create_dumb_t;               /* 32 bytes */

typedef struct {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
} drm_mode_map_dumb_t;                  /* 16 bytes */

typedef struct {
    uint32_t handle;
} drm_mode_destroy_dumb_t;              /* 4 bytes */

typedef struct {
    uint32_t fb_id;
    uint32_t width, height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
} drm_mode_fb_cmd_t;                    /* 28 bytes */

/* drm_mode.h: close fb (drmModeCloseFB) */
typedef struct {
    uint32_t fb_id;
    uint32_t pad;
} drm_mode_closefb_t;                   /* 8 bytes */

typedef struct {
    uint32_t fb_id;
    uint32_t width, height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
} drm_mode_fb_cmd2_t;                   /* 104 bytes */

typedef struct {
    uint64_t plane_id_ptr;
    uint32_t count_planes;
} drm_mode_get_plane_res_t;             /* 16 bytes */

typedef struct {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t possible_crtcs;
    uint32_t gamma_size;
    uint32_t count_format_types;
    uint64_t format_type_ptr;
} drm_mode_get_plane_t;                 /* 32 bytes */

typedef struct {
    uint64_t value;
    char     name[32];
} drm_mode_property_enum_t;             /* 40 bytes */

typedef struct {
    uint64_t values_ptr;
    uint64_t enum_blob_ptr;
    uint32_t prop_id;
    uint32_t flags;
    char     name[32];
    uint32_t count_values;
    uint32_t count_enum_blobs;
} drm_mode_get_property_t;              /* 64 bytes */

typedef struct {
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_props;
    uint32_t obj_id;
    uint32_t obj_type;
} drm_mode_obj_get_properties_t;        /* 32 bytes */

typedef struct {
    uint32_t flags;
    uint32_t count_objs;
    uint64_t objs_ptr;
    uint64_t count_props_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint64_t reserved;
    uint64_t user_data;
} drm_mode_atomic_t;                    /* 56 bytes */

/* drm_mode.h: connector property set (wlroots' legacy path turns DPMS on
 * before every SETCRTC) */
typedef struct {
    uint64_t value;
    uint32_t connector_id;
    uint32_t prop_id;
} drm_mode_connector_set_property_t;    /* 16 bytes */

/* drm_mode.h: property blob get (EDID, IN_FORMATS, ...) */
typedef struct {
    uint32_t blob_id;
    uint32_t length;
    uint64_t data;
} drm_mode_get_blob_t;                  /* 16 bytes */

/* drm_mode.h: object property set (atomic-style, used for VRR etc.) */
typedef struct {
    uint64_t value;
    uint32_t obj_id;
    uint32_t obj_type;
    uint32_t prop_id;
} drm_mode_obj_set_property_t;          /* 20 bytes */

#define DRM_IOCTL_MODE_GETRESOURCES  0xc04064a0
#define DRM_IOCTL_MODE_GETCRTC       0xc06864a1
#define DRM_IOCTL_MODE_SETCRTC       0xc06864a2
#define DRM_IOCTL_MODE_GETENCODER    0xc01464a6
#define DRM_IOCTL_MODE_GETCONNECTOR  0xc05064a7
#define DRM_IOCTL_MODE_GETPROPERTY   0xc04064aa
#define DRM_IOCTL_MODE_SETPROPERTY    0xc01064ab
#define DRM_IOCTL_MODE_GETPROPBLOB    0xc01064ac
#define DRM_IOCTL_MODE_GETFB          0xc01c64ad
#define DRM_IOCTL_MODE_ADDFB         0xc01c64ae
#define DRM_IOCTL_MODE_RMFB          0xc00464af
#define DRM_IOCTL_MODE_PAGE_FLIP     0xc01864b0
#define DRM_IOCTL_MODE_CREATE_DUMB   0xc02064b2
#define DRM_IOCTL_MODE_MAP_DUMB      0xc01064b3
#define DRM_IOCTL_MODE_DESTROY_DUMB  0xc00464b4
#define DRM_IOCTL_MODE_GETPLANERESOURCES 0xc01064b5
#define DRM_IOCTL_MODE_GETPLANE      0xc02064b6
#define DRM_IOCTL_MODE_ADDFB2        0xc06864b8
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES 0xc02064b9
#define DRM_IOCTL_MODE_OBJ_SETPROPERTY 0xc01464ba
#define DRM_IOCTL_MODE_ATOMIC        0xc03864bc
#define DRM_IOCTL_MODE_CLOSEFB       0xc00864c8

#define DRM_MODE_CONNECTOR_VGA        1
#define DRM_MODE_CONNECTOR_VIRTUAL   15
#define DRM_MODE_UNCONNECTED          0
#define DRM_MODE_CONNECTED            1
#define DRM_MODE_DISCONNECTED         2
#define DRM_MODE_UNKNOWNCONNECTION    3
#define DRM_MODE_ENCODER_NONE         0
#define DRM_MODE_ENCODER_DAC          1
#define DRM_MODE_ENCODER_TMDS         2
#define DRM_MODE_ENCODER_VIRTUAL      5
#define DRM_MODE_FLAG_PHSYNC          1
#define DRM_MODE_FLAG_NHSYNC          2
#define DRM_MODE_FLAG_PVSYNC          4
#define DRM_MODE_FLAG_NVSYNC          8
#define DRM_MODE_TYPE_BUILTIN         1
#define DRM_MODE_SUBPIXEL_UNKNOWN     0
#define DRM_MODE_SUBPIXEL_HORIZONTAL_RGB 1

/* drm_mode.h: page flip flags */
#define DRM_MODE_PAGE_FLIP_EVENT       0x01
#define DRM_MODE_PAGE_FLIP_ASYNC       0x02

#define DRM_FORMAT_XRGB8888 0x34325258u /* 'XR24' little-endian */
#define DRM_FORMAT_ARGB8888 0x34324152u /* 'AR24' */
#define DRM_FORMAT_MOD_LINEAR 0

/* drm.h */
#define DRM_CLIENT_CAP_STEREO_3D       1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_CLIENT_CAP_ATOMIC          3
#define DRM_CAP_PRIME                  0x05
#define DRM_CAP_TIMESTAMP_MONOTONIC    6
#define DRM_CAP_CRTC_IN_VBLANK_EVENT   0x12
#define DRM_CAP_ADDFB2_MODIFIERS       0x10
#define DRM_PRIME_CAP_IMPORT           1
#define DRM_PRIME_CAP_EXPORT           2

/* drm.h: kernel->user events (read back from the drm fd).  wlroots runs the
 * legacy uAPI and waits for DRM_EVENT_FLIP_COMPLETE after every page flip,
 * so this is the event the flip ioctl queues. */
#define DRM_EVENT_VBLANK               0x01
#define DRM_EVENT_FLIP_COMPLETE        0x01

typedef struct {
    uint32_t length;                /* sizeof(struct drm_event) */
    uint32_t type;
} drm_event_t;                      /* 8 bytes */

typedef struct {
    drm_event_t base;               /* length = 24, type = FLIP_COMPLETE */
    uint64_t    user_data;
    uint32_t    tv_sec;
    uint32_t    tv_usec;
    uint32_t    sequence;
    uint32_t    crtc_id;
} drm_event_vblank_t;               /* 32 bytes */

/* drm_mode.h */
#define DRM_MODE_FB_MODIFIERS          (1 << 1) /* enables modifier[] */
#define DRM_MODE_PROP_RANGE            (1 << 1)
#define DRM_MODE_PROP_IMMUTABLE        (1 << 2)
#define DRM_MODE_PROP_ENUM             (1 << 3)
#define DRM_MODE_PROP_BLOB             (1 << 4)
#define DRM_MODE_PROP_ATOMIC           0x80000000
#define DRM_MODE_OBJECT_ANY           0
#define DRM_MODE_OBJECT_CRTC           0xccccccccu
#define DRM_MODE_OBJECT_CONNECTOR      0xc0c0c0c0u
#define DRM_MODE_OBJECT_ENCODER        0xe0e0e0e0u
#define DRM_MODE_OBJECT_PLANE          0xeeeeeeeeu

/* Object ids and property ids this driver hands out (kernel-side, not UAPI).
 * crtc 1 and connector 1 also appear in GETRESOURCES/GETCONNECTOR. */
#define DRM_PROP_ID_DPMS               1
#define DRM_PROP_ID_EDID               2
#define DRM_PROP_ID_PLANE_TYPE         3
#define DRM_BLOB_ID_EDID               1

/* ---- kernel-side entry (not part of the UAPI) ---------------------------- */
#include "bootinfo.h"

/* Publish /dev/dri/card0 and /dev/dri/renderD128 and drive the bochs VBE.
 * Returns 1 when the devices came up, 0 otherwise. */
int drm_init(const bootinfo_t *bi);

#endif
