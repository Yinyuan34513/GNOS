/*
 * drm_devtmpfs.c — GNOS bridge for Uinxed's devtmpfs_register_char_device.
 * (GPLv2)
 *
 * Uinxed's DRM core registers its nodes with devtmpfs_register_char_device()
 * passing a tmpfs_device_ops_t table of per-open callbacks.  GNOS's VFS has
 * no devtmpfs and no per-open private-data callbacks; it publishes /dev
 * nodes with vfs_register_devnum() and dispatches through a flat vfs_ops_t
 * keyed on the node.  This file converts one into the other:
 *
 *   - the ioctl/mmap/read/poll trio is adapted to the GNOS vfs_ops_t
 *     signatures, forwarding to the Uinxed table with its ctx;
 *   - open/release (per-open state) are not representable in the GNOS
 *     model: the singleton device keeps one file state in the node's priv,
 *     and drm_init.c's open/release callbacks are invoked from the GNOS
 *     DRM entry points instead of from a VFS callback.
 *
 * The path argument is "/dev/dri/card0"-style; vfs_register_devnum wants
 * the name relative to /dev ("dri/card0").
 */
#include "drm_devtmpfs.h"
#include "kstring.h"
#include "drm_device.h"     /* drm_open */
#include "drm_init.h"       /* drm_get_singleton */

extern uint64_t g_hhdm;    /* HHDM offset, defined in kernel.c */

/* GNOS's VFS has no per-open callbacks, so the ported per-open drm_file
 * cannot be created on open().  Keep one lazily-initialised file for the
 * whole system instead: the kernel is single-user with one DRM device and
 * one compositor, which is exactly the shape drm_open() serves. */
static struct drm_file *g_drm_file;

static struct drm_file *drm_get_file(void)
{
    struct drm_device *dev;

    if (g_drm_file)
        return g_drm_file;

    dev = drm_get_singleton();
    if (!dev)
        return NULL;
    g_drm_file = malloc(sizeof(*g_drm_file));
    if (!g_drm_file)
        return NULL;
    memset(g_drm_file, 0, sizeof(*g_drm_file));
    if (drm_open(dev, g_drm_file)) {
        free(g_drm_file);
        g_drm_file = NULL;
        return NULL;
    }
    /* Root compositor on the primary node is already trusted for DRM_AUTH
     * ioctls (drm_dev_open does the same for devtmpfs). */
    g_drm_file->authenticated = true;
    return g_drm_file;
}

/* GNOS vfs_ops_t callbacks forwarding into the Uinxed table. */
static int32_t drm_bridge_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    tmpfs_device_ops_t *ops = (tmpfs_device_ops_t *)n->priv;
    struct drm_file    *fp  = drm_get_file();

    dbg_puts("DRMIOC: cmd=0x");
    dbg_puts_hex(cmd);
    dbg_puts(" ops=");
    dbg_puts_hex((uint64_t)(uintptr_t)ops);
    dbg_puts(" fp=");
    dbg_puts_hex((uint64_t)(uintptr_t)fp);
    dbg_puts("\r\n");

    if (!ops || !ops->file_ioctl)
        return -E_NOTTY;
    if (!fp)
        return -E_NOMEM;
    return ops->file_ioctl(ops->ctx, fp, 0, (size_t)cmd, (void *)(uintptr_t)arg);
}

static int drm_bridge_mmap(vfs_node_t *n, uint64_t offset, uint64_t *phys,
                           uint64_t *size)
{
    tmpfs_device_ops_t *ops = (tmpfs_device_ops_t *)n->priv;
    struct drm_file    *fp  = drm_get_file();
    struct vm_area      fake_vma = {0};   /* GNOS has no VMAs; Uinxed's
                                             mmap callback wants one anyway */
    void               *p;

    dbg_puts("DRMBRIDGE: n=");
    dbg_puts_hex((uint64_t)(uintptr_t)n);
    dbg_puts(" off=");
    dbg_puts_hex(offset);
    dbg_puts(" ops=");
    dbg_puts_hex((uint64_t)(uintptr_t)ops);
    dbg_puts(" mmapfn=");
    dbg_puts_hex((uint64_t)(uintptr_t)(ops ? ops->mmap : 0));
    dbg_puts(" ctx=");
    dbg_puts_hex((uint64_t)(uintptr_t)(ops ? ops->ctx : 0));
    dbg_puts(" readfn=");
    dbg_puts_hex((uint64_t)(uintptr_t)(ops ? ops->read : 0));
    dbg_puts("\r\n");

    if (!ops || !ops->mmap)
        return -E_INVAL;
    if (!fp)
        return -E_NOMEM;
    p = ops->mmap(ops->ctx, fp, (uint64_t)offset, 0, 0, &fake_vma);
    if (!p)
        return -E_INVAL;
    /* p is a kernel-virtual (HHDM) pointer to the backing memory; sys_mmap
     * needs the *physical* address to build the user PTEs, so convert it. */
    *phys = ((uint64_t)(uintptr_t)p >= g_hhdm)
                ? (uint64_t)(uintptr_t)p - g_hhdm
                : (uint64_t)(uintptr_t)p;
    if (size) {
        /* Report the real GEM buffer size so sys_mmap() maps the whole
         * span; returning 0 would clamp the request to zero and leave the
         * mapping with no PTEs (user write -> page fault). */
        struct drm_gem_object *obj = drm_gem_object_lookup_by_offset(fp, offset);
        *size = obj ? obj->size : 0;
        if (obj)
            drm_gem_object_put(obj);
    }
    return 0;
}

static int32_t drm_bridge_read(vfs_node_t *n, uint64_t off, void *buf,
                               uint32_t len)
{
    tmpfs_device_ops_t *ops = (tmpfs_device_ops_t *)n->priv;

    if (!ops || !ops->file_read)
        return 0;
    return (int32_t)ops->file_read(ops->ctx, NULL, 0, buf, (size_t)off,
                                   (size_t)len);
}

static int32_t drm_bridge_write(vfs_node_t *n, uint64_t off, const void *buf,
                                uint32_t len)
{
    tmpfs_device_ops_t *ops = (tmpfs_device_ops_t *)n->priv;

    if (!ops || !ops->file_write)
        return 0;
    return (int32_t)ops->file_write(ops->ctx, NULL, 0, buf, (size_t)off,
                                    (size_t)len);
}

static int drm_bridge_poll(vfs_node_t *n, int16_t events, int16_t *revents)
{
    tmpfs_device_ops_t *ops = (tmpfs_device_ops_t *)n->priv;
    int                 r;

    if (!ops || !ops->file_poll)
        return 0;
    r = ops->file_poll(ops->ctx, NULL, 0, (size_t)events);
    *revents = (int16_t)(events & (POLLIN | POLLOUT));
    if (r != 0)
        *revents = 0;
    return 0;
}

int devtmpfs_register_char_device(const char *path, uint64_t devt,
                                  uint64_t devt2, int file_stream_flags,
                                  tmpfs_device_ops_t *ops)
{
    static const vfs_ops_t g_drm_ops = {
        .ioctl = drm_bridge_ioctl,
        .mmap  = drm_bridge_mmap,
        .read  = drm_bridge_read,
        .write = drm_bridge_write,
        .poll  = drm_bridge_poll,
    };
    char name[64];
    uint32_t maj = (uint32_t)((devt >> 8) & 0xfff);
    uint32_t min = (uint32_t)((devt & 0xff) | ((devt >> 12) & 0xfffff00));

    (void)devt2;
    (void)file_stream_flags;

    if (!path || path[0] != '/' || !ops)
        return -E_INVAL;

    /* Uinxed's caller hands us a stack-local tmpfs_device_ops_t; the VFS
     * keeps the pointer in the node's priv, so it must survive the caller's
     * frame.  Copy it to the kernel heap. */
    tmpfs_device_ops_t *keep = kmalloc(sizeof(*keep));
    if (!keep)
        return -E_NOMEM;
    memcpy(keep, ops, sizeof(*keep));

    /* "/dev/dri/card0" -> "dri/card0" */
    if (strncmp(path, "/dev/", 5) == 0)
        path += 5;
    if (strlen(path) >= sizeof name) {
        kfree(keep);
        return -E_NAMETOOLONG;
    }
    memcpy(name, path, strlen(path) + 1);

    return vfs_register_devnum(name, &g_drm_ops, keep, maj, min);
}
