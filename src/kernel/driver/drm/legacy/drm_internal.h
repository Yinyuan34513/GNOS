/*
 * drm_internal.h — shared state and prototypes for the GNOS DRM driver,
 * split across the drm/ modules. (GPLv2)
 *
 * The UAPI structs and ioctl numbers live in drm.h and are byte-copies of
 * the Linux UAPI (do not touch); everything the modules need to share is
 * declared here.
 */
#ifndef GNOS_DRM_INTERNAL_H
#define GNOS_DRM_INTERNAL_H

#include "drm.h"
#include "vfs.h"        /* vfs_node_t, vfs_ops_t */

/* ---- current scanout state (drm_vbe.c) ----------------------------------- */
extern int g_vbe;                       /* VBE_DISPI answered the ID probe */
extern uint32_t g_cur_w, g_cur_h;       /* current mode */
extern uint32_t g_cur_fb;               /* fb id on screen, 0 = console owns it */

/* ---- the mode list (drm_modes.c) ------------------------------------------ */
typedef struct {
    const char *name;
    uint32_t    h, v;
} mode_t;

#define N_MODES 4

extern const mode_t g_modes[N_MODES];

int  boot_mode_known(uint32_t *index);
void fill_modeinfo(drm_mode_modeinfo_t *m, const mode_t *src);
const mode_t *mode_by_size(uint32_t w, uint32_t h);
void current_modeinfo(drm_mode_modeinfo_t *m);

/* ---- VBE_DISPI register interface (drm_vbe.c) ------------------------------ */
#define VBE_PORT_IDX  0x01CE
#define VBE_PORT_DAT  0x01CF
#define VBE_ID      0x00
#define VBE_XRES    0x01
#define VBE_YRES    0x02
#define VBE_BPP     0x03
#define VBE_ENABLE  0x04
#define VBE_ENABLE_LFB 0x41             /* enable + linear framebuffer */

void vbe_write(uint16_t idx, uint16_t val);
uint16_t vbe_read(uint16_t idx);

/* ---- dumb buffers (drm_dumb.c) --------------------------------------------- */
#define MAX_DUMB 8

typedef struct {
    int      used;
    uint32_t handle;
    uint64_t phys;
    uint32_t size;      /* bytes */
    uint32_t pitch;
    uint32_t w, h;
} dumb_t;

extern dumb_t g_dumb[MAX_DUMB];
dumb_t *dumb_by_handle(uint32_t h);

/* ---- framebuffers (drm_framebuffer.c) -------------------------------------- */
#define MAX_FB 8

typedef struct {
    int      used;
    uint32_t fb_id;
    uint32_t width, height, pitch;
    uint32_t handle;
} fb_t;

extern fb_t g_fbs[MAX_FB];
fb_t *fb_by_id(uint32_t id);
void blit_fb(uint64_t phys, uint32_t src_pitch, uint32_t w, uint32_t h,
             uint32_t x, uint32_t y);
void blit_fb_virt(const void *src, uint32_t src_pitch, uint32_t w,
                  uint32_t h, uint32_t x, uint32_t y);

/* ---- user-memory helpers (drm_ioctl.c) ------------------------------------- */
int copy_from_user(void *k, uint64_t u, uint64_t n);
int copy_to_user(uint64_t u, const void *k, uint64_t n);

/* ---- ioctl handlers, one module per family --------------------------------- */
/* drm_auth.c: client identity and capabilities */
int32_t drm_ioctl_version(uint64_t arg);
int32_t drm_ioctl_get_unique(uint64_t arg);
int32_t drm_ioctl_get_cap(uint64_t arg);
int32_t drm_ioctl_set_client_cap(uint64_t arg);
int32_t drm_ioctl_get_magic(uint64_t arg);
int32_t drm_ioctl_auth_magic(uint64_t arg);
int32_t drm_ioctl_set_master(uint64_t arg);
int32_t drm_ioctl_drop_master(uint64_t arg);
int32_t drm_ioctl_prime_handle_to_fd(uint64_t arg);
int32_t drm_ioctl_prime_fd_to_handle(uint64_t arg);
int32_t drm_ioctl_gem_close(uint64_t arg);
/* drm_kms.c: resources / connector / encoder / crtc / plane / page flip */
int32_t drm_ioctl_get_resources(uint64_t arg);
int32_t drm_ioctl_get_connector(uint64_t arg);
int32_t drm_ioctl_get_encoder(uint64_t arg);
int32_t drm_ioctl_get_crtc(uint64_t arg);
int32_t drm_ioctl_set_crtc(uint64_t arg);
int32_t drm_ioctl_get_plane_res(uint64_t arg);
int32_t drm_ioctl_get_plane(uint64_t arg);
int32_t drm_ioctl_page_flip(uint64_t arg);
/* drm_property.c: properties and atomic */
int32_t drm_ioctl_get_property(uint64_t arg);
int32_t drm_ioctl_obj_get_properties(uint64_t arg);
int32_t drm_ioctl_atomic(uint64_t arg);
int32_t drm_ioctl_set_property(uint64_t arg);
int32_t drm_ioctl_obj_set_property(uint64_t arg);
int32_t drm_ioctl_get_prop_blob(uint64_t arg);
/* drm_dumb.c */
int32_t drm_ioctl_create_dumb(uint64_t arg);
int32_t drm_ioctl_map_dumb(uint64_t arg);
int32_t drm_ioctl_destroy_dumb(uint64_t arg);
/* drm_framebuffer.c */
int32_t drm_ioctl_getfb(uint64_t arg);
int32_t drm_ioctl_addfb(uint64_t arg);
int32_t drm_ioctl_addfb2(uint64_t arg);
int32_t drm_ioctl_rmfb(uint64_t arg);
int32_t drm_ioctl_closefb(uint64_t arg);
int32_t drm_addfb(uint32_t width, uint32_t height, uint32_t pitch,
                  uint32_t bpp, uint32_t depth, uint32_t handle,
                  uint32_t *out_id);

/* ---- kernel->user event queue (drm_event.c) -------------------------------- */
void drm_queue_event(uint64_t user_data);
int32_t drm_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len);
int drm_poll(vfs_node_t *n, int16_t events, int16_t *revents);

/* ---- ioctl dispatch (drm_ioctl.c) and device registration (drm_init.c) ------ */
int32_t drm_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg);
int drm_mmap(vfs_node_t *n, uint64_t offset, uint64_t *phys, uint64_t *size);
extern const vfs_ops_t g_drm_ops;

#endif
