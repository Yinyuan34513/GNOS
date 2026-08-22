/*
 * drm_ioctl.c — the ioctl dispatch table, user-memory helpers and mmap.
 * (GPLv2)
 *
 * The render node (/dev/dri/renderD128) has no scanout: it exists for
 * buffer allocation and rendering, and libdrm's drmIsKMS() probes exactly
 * this by asking for the mode resources.  Refuse the modeset family here so
 * a render node is not mistaken for a second GPU -- wlroots would otherwise
 * create a second DRM backend for /dev/dri/renderD128, and both would fight
 * over crtc 1.  The dumb-buffer and ADDFB2 ioctls stay valid: they are
 * render-node ops.
 */
#include "drm_internal.h"
#include "vmm.h"
#include "kstring.h"

/* ---- user-memory helpers (same style as fbdev) ---------------------------- */
int copy_from_user(void *k, uint64_t u, uint64_t n)
{
    if (!user_ptr_ok(u, n))
        return -E_FAULT;
    memcpy(k, (const void *)(uintptr_t)u, n);
    return 0;
}

int copy_to_user(uint64_t u, const void *k, uint64_t n)
{
    if (!user_ptr_ok(u, n))
        return -E_FAULT;
    memcpy((void *)(uintptr_t)u, k, n);
    return 0;
}

/* ---- ioctl dispatch -------------------------------------------------------- */
static int drm_is_kms_ioctl(uint64_t cmd)
{
    uint32_t nr = cmd & 0xFF;
    if (nr < 0xA0 || nr > 0xBF)
        return 0;
    switch (nr) {
    case 0xB2: case 0xB3: case 0xB4:   /* dumb create/map/destroy */
    case 0xB8:                          /* addfb2 */
        return 0;
    }
    return 1;
}

int32_t drm_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    if (n && strcmp(n->name, "dri/renderD128") == 0 && drm_is_kms_ioctl(cmd))
        return -E_OPNOTSUPP;
    if ((cmd & 0xFF) == 0xC6)   /* DRM_IOCTL_MODE_CREATE_LEASE: no leasing */
        return -E_OPNOTSUPP;
    switch (cmd) {
    case DRM_IOCTL_VERSION:        return drm_ioctl_version(arg);
    case DRM_IOCTL_GET_UNIQUE:     return drm_ioctl_get_unique(arg);
    case DRM_IOCTL_GET_CAP:        return drm_ioctl_get_cap(arg);
    case DRM_IOCTL_SET_CLIENT_CAP: return drm_ioctl_set_client_cap(arg);
    /* The magic-number dance is how libdrm's drmIsMaster() and wlroots'
     * allocator verify DRM master.  GNOS has no master concept: every
     * opener is master, so GET_MAGIC hands out a token and AUTH_MAGIC
     * accepts it.  drmIsMaster() then reports true and wlroots uses the
     * (working) dumb-buffer allocator instead of giving up. */
    case DRM_IOCTL_GET_MAGIC:      return drm_ioctl_get_magic(arg);
    case DRM_IOCTL_AUTH_MAGIC:     return drm_ioctl_auth_magic(arg);
    /* Master acquisition is a no-op: GNOS has no master concept, every
     * opener is master, so wlroots' drmDropMaster() on device teardown must
     * succeed rather than log "Failed to drop master: Not a tty". */
    case DRM_IOCTL_SET_MASTER:     return drm_ioctl_set_master(arg);
    case DRM_IOCTL_DROP_MASTER:    return drm_ioctl_drop_master(arg);
    case DRM_IOCTL_PRIME_HANDLE_TO_FD: return drm_ioctl_prime_handle_to_fd(arg);
    case DRM_IOCTL_PRIME_FD_TO_HANDLE: return drm_ioctl_prime_fd_to_handle(arg);
    case DRM_IOCTL_GEM_CLOSE:      return drm_ioctl_gem_close(arg);
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
    case DRM_IOCTL_MODE_GETFB:        return drm_ioctl_getfb(arg);
    case DRM_IOCTL_MODE_RMFB:         return drm_ioctl_rmfb(arg);
    case DRM_IOCTL_MODE_CLOSEFB:      return drm_ioctl_closefb(arg);
    case DRM_IOCTL_MODE_SETPROPERTY:  return drm_ioctl_set_property(arg);
    case DRM_IOCTL_MODE_OBJ_SETPROPERTY: return drm_ioctl_obj_set_property(arg);
    case DRM_IOCTL_MODE_GETPROPBLOB:  return drm_ioctl_get_prop_blob(arg);
    case DRM_IOCTL_MODE_PAGE_FLIP:    return drm_ioctl_page_flip(arg);
    default:
        return -E_NOTTY;
    }
}

/* mmap(): offset >> PAGE_SHIFT names the dumb buffer, the Linux way. */
int drm_mmap(vfs_node_t *n, uint64_t offset, uint64_t *phys,
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
