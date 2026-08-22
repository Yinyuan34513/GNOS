/*
 * drm_devtmpfs.h — GNOS shim for Uinxed's device-model headers.
 * (GPLv2)
 *
 * Uinxed registers DRM nodes through its devtmpfs + device/class/kobject
 * machinery (fs/virtual/devtmpfs.h, drivers/base/device.h); GNOS publishes
 * /dev nodes with vfs_register_devnum() instead and has no sysfs at all.
 * This header gives the ported DRM files the handful of Uinxed types and
 * entry points they touch, with the registration bridged onto the GNOS VFS.
 *
 * Sysfs-facing pieces (struct device/class/kobject, device_create,
 * class_register, uevent) are reduced to inert stubs: nothing in GNOS
 * walks /sys, so they have no caller-visible effect.
 */
#ifndef GNUCOS_DRM_DEVTMPS_H
#define GNUCOS_DRM_DEVTMPS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "vfs.h"        /* vfs_node_t, vfs_ops_t, vfs_register_devnum */
#include "proc.h"       /* proc_t, proc_current */
#include "smp.h"        /* spinlock_t */
#include "drm_port.h"   /* kmalloc/kfree/errno maps */

/* ---- struct vm_area (Uinxed proc/process.h) -------------------------
 * GNOS's mmap does not build VMAs; the ported drm_init.c mmap callbacks
 * receive a NULL vma and must not dereference it.  The struct exists only
 * so the Uinxed signatures type-check; the one field drm_init.c writes is
 * kept for layout fidelity. */
struct vm_area {
    void *vm_private_data;
};

/* ---- process_t / process_current() (Uinxed proc/process.h) ---------- */
typedef proc_t process_t;

static inline process_t *process_current(void)
{
    return proc_current();
}

/* ---- sysfs stubs (drivers/base/device.h) ---------------------------- */
struct kobject {
    const char *name;
};
struct device {
    struct kobject kobj;
};
struct kobj_uevent_env {
    int dummy;
};
struct class {
    const char *name;
    int (*dev_uevent)(struct device *dev, struct kobj_uevent_env *env);
};

static inline void *device_create(struct class *cls, void *parent,
                                  uint64_t devt, void *drvdata,
                                  const char *fmt, ...)
{
    (void)cls; (void)parent; (void)devt; (void)drvdata; (void)fmt;
    return NULL;            /* GNOS has no sysfs; nothing to create */
}

static inline int class_register(struct class *cls)
{
    (void)cls;
    return 0;
}

static inline const char *kobject_name(const struct kobject *k)
{
    return k ? k->name : "";
}

static inline int add_uevent_var(struct kobj_uevent_env *env, const char *fmt, ...)
{
    (void)env; (void)fmt;
    return 0;
}

/* ---- devtmpfs registration (fs/virtual/devtmpfs.h) ------------------ */
/* Uinxed device-operation table.  The open/release/mmap/file_* members use
 * Uinxed callback signatures; the bridge below converts the ioctl/mmap/read
 * trio into the GNOS vfs_ops_t callbacks at registration time. */
typedef size_t (*tmpfs_dev_read_t)(void *ctx, void *addr, size_t offset, size_t size);
typedef size_t (*tmpfs_dev_write_t)(void *ctx, const void *addr, size_t offset, size_t size);
typedef int (*tmpfs_dev_poll_t)(void *ctx, size_t events);
typedef int (*tmpfs_dev_ioctl_t)(void *ctx, size_t req, void *arg);
typedef int (*tmpfs_dev_open_t)(vfs_node_t node, uint64_t flags, void **private_data);
typedef void (*tmpfs_dev_release_t)(vfs_node_t node, void *private_data);
typedef void *(*tmpfs_dev_mmap_t)(void *ctx, void *private_data, uint64_t offset,
                                  uint64_t size, int flags, struct vm_area *vma);
typedef int64_t (*tmpfs_dev_file_read_t)(void *ctx, void *private_data,
                                         uint64_t flags, void *addr,
                                         size_t offset, size_t size);
typedef int64_t (*tmpfs_dev_file_write_t)(void *ctx, void *private_data,
                                          uint64_t flags, const void *addr,
                                          size_t offset, size_t size);
typedef int (*tmpfs_dev_file_poll_t)(void *ctx, void *private_data,
                                     uint64_t flags, size_t events);
typedef int (*tmpfs_dev_file_ioctl_t)(void *ctx, void *private_data,
                                      uint64_t flags, size_t req, void *arg);
typedef void (*tmpfs_dev_destroy_t)(void *ctx);

typedef struct {
    tmpfs_dev_read_t       read;
    tmpfs_dev_write_t      write;
    tmpfs_dev_poll_t       poll;
    tmpfs_dev_ioctl_t      ioctl;
    tmpfs_dev_open_t       open;
    tmpfs_dev_release_t    release;
    tmpfs_dev_mmap_t       mmap;
    tmpfs_dev_file_read_t  file_read;
    tmpfs_dev_file_write_t file_write;
    tmpfs_dev_file_poll_t  file_poll;
    tmpfs_dev_file_ioctl_t file_ioctl;
    tmpfs_dev_destroy_t    destroy;
    void                  *ctx;
} tmpfs_device_ops_t;

/* Uinxed file-stream flags; GNOS does not model them, keep the name. */
enum { file_stream = 0x8UL };

/* Register a character device node at /dev/<rel> with major/minor @maj/@min,
 * bridging @ops onto the GNOS VFS.  Returns 0 on success, negative errno. */
int devtmpfs_register_char_device(const char *path, uint64_t devt,
                                  uint64_t devt2, int file_stream_flags,
                                  tmpfs_device_ops_t *ops);

/* ---- misc Uinxed helpers -------------------------------------------- */
static inline uint64_t MKDEV(uint32_t maj, uint32_t min)
{
    /* Linux new_encode_dev(), same encoding vfs_register_devnum uses. */
    return (min & 0xff) | (maj << 8) | ((min & ~0xffu) << 12);
}

/* strdup is supplied by drm_libc.c (declared in drm_port.h). */

#define EOK 0

#endif /* GNUCOS_DRM_DEVTMPS_H */
