# ported/ — Uinxed-Kernel DRM core, adapted for GNOS

This directory is a port of the DRM subsystem from
[Uinxed-Kernel](https://github.com/OpenXJ380/Uinxed-Kernel)
(`OpenXJ380/Uinxed-Kernel`), a freestanding x86_64 teaching kernel.

## Source

Every file under `ported/` that carries the `Copyright 2020 ViudiraTech,
based on the Apache 2.0 license` header was copied verbatim from
Uinxed-Kernel and then adapted to GNOS's kernel facilities.  The original
copyright and license headers are preserved in each file; the adaptation
shims (below) are new code written for this port.

| File(s) in ported/ | Original path in Uinxed-Kernel |
|--------------------|--------------------------------|
| drm_atomic.c, drm_atomic_helper.c, drm_atomic_uapi.c | `drivers/gpu/drm/` |
| drm_auth.c, drm_blend.c, drm_connector.c, drm_crtc.c | `drivers/gpu/drm/` |
| drm_drv.c, drm_encoder.c, drm_file.c, drm_framebuffer.c | `drivers/gpu/drm/` |
| drm_gem.c, drm_hashtab.c, drm_idr.c, drm_init.c | `drivers/gpu/drm/` |
| drm_ioctl.c, drm_mm.c, drm_mode_config.c, drm_mode_object.c | `drivers/gpu/drm/` |
| drm_modes.c, drm_modeset_lock.c, drm_plane.c, drm_print.c | `drivers/gpu/drm/` |
| drm_property.c, drm_rect.c, drm_vblank.c | `drivers/gpu/drm/` |
| drm.h, drm_mode.h, drm_fourcc.h, drm_color_mgmt.h | `include/drivers/gpu/` |
| drm_device.h, drm_hashtab.h, drm_idr.h, drm_init.h | `include/drivers/gpu/` |
| drm_mm.h, drm_mode.h, drm_modeset_lock.h, drm_print.h | `include/drivers/gpu/` |
| drm_rect.h | `include/drivers/gpu/` |
| intrusive_list.c/h | `libs/glist/` |
| rbtree.c/h | `libs/data/` |

## Adaptation shims (new code for GNOS)

These files are written for this port, not copied: they map Uinxed's kernel
facilities onto GNOS equivalents so the ported code compiles and runs
unchanged.  They are original work under the GNOS project license.

* `drm_port.h` — errno spelling (ENOENT→E_NOENT), malloc→kmalloc,
  copy_from_user/copy_to_user wrappers, wait_queue_t + wait_queue_*,
  minimal stdbool/stdarg/offsetof/UINT16_MAX fills for the freestanding
  kernel headers.
* `drm_vsnprintf.c/h` — minimal vsnprintf/snprintf/plogk (GNOS's kernel
  has no snprintf; procfs formats numbers by hand).
* `drm_devtmpfs.h/c` — devtmpfs/device/class/kobject shim: maps Uinxed's
  devtmpfs_register_char_device() onto GNOS's vfs_register_devnum().
* `drm_devtmpfs.h` — process_t/process_current(), struct vm_area,
  MKDEV(), strdup() aliases onto GNOS's proc/vfs/string facilities.

## GNOS integration notes

* `kthread_create()` / `kthread_bootstrap()` live in
  `src/kernel/core/proc.c` (entry via `kthread_trampoline` in
  `switch.asm`); the GNOS scheduler's `sched_block`/`sched_wake_queue`
  back the Uinxed wait_queue calls.
* `vmm_kernel_as()` in `src/kernel/core/vmm.c` gives kernel threads a
  shared address space over the kernel PML4.
* The old GNOS DRM implementation was moved to
  `src/kernel/driver/drm/legacy/` when the port took over.
