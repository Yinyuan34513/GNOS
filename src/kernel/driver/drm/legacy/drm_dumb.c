/*
 * drm_dumb.c — dumb buffer allocation for /dev/dri. (GPLv2)
 *
 * Dumb buffers are physically contiguous frames handed out by the page
 * allocator; MAP_DUMB encodes the handle in the mmap offset exactly as
 * Linux does (offset = handle << PAGE_SHIFT), so an unmodified dumb-buffer
 * client's mmap() lands on the right memory.
 */
#include "drm_internal.h"
#include "pmm.h"
#include "kstring.h"
#include "anonfd.h"
#include "syscall.h"

dumb_t g_dumb[MAX_DUMB];

dumb_t *dumb_by_handle(uint32_t h)
{
    for (int i = 0; i < MAX_DUMB; i++)
        if (g_dumb[i].used && g_dumb[i].handle == h)
            return &g_dumb[i];
    return NULL;
}

int32_t drm_ioctl_create_dumb(uint64_t arg)
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

int32_t drm_ioctl_map_dumb(uint64_t arg)
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

int32_t drm_ioctl_destroy_dumb(uint64_t arg)
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

/* GEM_CLOSE: the compositor drops its per-fd handle reference here (wlroots
 * calls it from drm_fb_destroy).  GNOS has no per-fd GEM handle table -- a
 * dumb buffer's pages are owned by the buffer itself and freed by
 * DESTROY_DUMB -- so closing the handle is a no-op that must simply succeed;
 * failing it makes wlroots log "drmCloseBufferHandle failed". */
int32_t drm_ioctl_gem_close(uint64_t arg)
{
    drm_gem_close_t c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -E_FAULT;
    return 0;
}

/* ---- PRIME: dumb buffer <-> dmabuf fd ---------------------------------
 * wlroots' dumb allocator exports each buffer as a PRIME fd (it builds a
 * wlr_dmabuf_attributes from it for the swapchain), and the DRM backend
 * later imports that fd back into a handle for ADDFB2.  Both directions
 * must round-trip through the same dumb buffer.
 *
 * The fd handed out here is an anonymous node whose mmap maps the dumb
 * buffer's physical span; PRIME_FD_TO_HANDLE recovers the handle from the
 * node's priv.  No reference counting: the dumb buffer is owned by
 * DESTROY_DUMB, and the compositor closes the fd before destroying the
 * buffer. */

static int32_t drm_prime_mmap(vfs_node_t *n, uint64_t offset, uint64_t *phys,
                              uint64_t *size)
{
    dumb_t *d = (dumb_t *)n->priv;
    if (offset & 0xFFF)
        return -E_INVAL;
    if (offset >= d->size)
        return -E_INVAL;
    *phys = d->phys + offset;
    *size = d->size - offset;
    return 0;
}

static int drm_prime_poll(vfs_node_t *n, int16_t events, int16_t *revents)
{
    (void)n;
    *revents = (int16_t)(events & (POLLIN | POLLOUT));
    return 0;
}

static const vfs_ops_t g_drm_prime_ops = {
    .mmap = drm_prime_mmap,
    .poll = drm_prime_poll,
};

int32_t drm_ioctl_prime_handle_to_fd(uint64_t arg)
{
    drm_prime_handle_t p;
    if (copy_from_user(&p, arg, sizeof p) < 0)
        return -E_FAULT;
    dumb_t *d = dumb_by_handle(p.handle);
    if (!d)
        return -E_NOENT;

    int h = vfs_anon_open(VFS_ANON, &g_drm_prime_ops, d, O_RDWR);
    if (h < 0)
        return h;
    int fd = anon_bind(h, (p.flags & DRM_CLOEXEC) != 0);
    if (fd < 0)
        return fd;
    p.fd = fd;
    return copy_to_user(arg, &p, sizeof p);
}

int32_t drm_ioctl_prime_fd_to_handle(uint64_t arg)
{
    drm_prime_handle_t p;
    if (copy_from_user(&p, arg, sizeof p) < 0)
        return -E_FAULT;
    int h = fd_handle(p.fd);
    if (h < 0)
        return -E_BADF;
    vfs_node_t *n = (vfs_node_t *)vfs_file_node(h);
    if (!n || n->kind != VFS_ANON || n->ops != &g_drm_prime_ops)
        return -E_INVAL;
    p.handle = ((dumb_t *)n->priv)->handle;
    return copy_to_user(arg, &p, sizeof p);
}
