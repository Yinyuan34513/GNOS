/*
 * drm_auth.c — client identity and capabilities ioctls. (GPLv2)
 *
 * GNOS has no master concept: every opener is master.  GET_MAGIC hands out
 * a token and AUTH_MAGIC accepts it, so libdrm's drmIsMaster() and
 * wlroots' allocator report master and use the (working) dumb-buffer
 * allocator instead of giving up; SET/DROP_MASTER are no-ops so wlroots'
 * drmDropMaster() on teardown succeeds rather than log
 * "Failed to drop master: Not a tty".
 */
#include "drm_internal.h"

int32_t drm_ioctl_version(uint64_t arg)
{
    drm_version_t v;
    if (copy_from_user(&v, arg, sizeof v) < 0)
        return -E_FAULT;
    static const char name[] = "gnos-drm";
    static const char date[] = "20250101";
    static const char desc[] = "GNOS bochs-display DRM driver";
    v.version_major = 1; v.version_minor = 0; v.version_patchlevel = 0;
    /* The lengths must come back filled in: libdrm's drmGetVersion() makes
     * the first ioctl with NULL buffers purely to learn name_len/date_len/
     * desc_len, mallocs exactly that much, and then calls again.  A driver
     * that leaves them at 0 makes libdrm leave name == NULL, and wlroots'
     * "Initializing DRM backend for %s (%s)" does strlen() on it -- a NULL
     * deref on the very first compositor frame. */
    v.name_len = sizeof name - 1;
    v.date_len = sizeof date - 1;
    v.desc_len = sizeof desc - 1;
    if (v.name && v.name_len)
        copy_to_user((uint64_t)(uintptr_t)v.name, name, v.name_len);
    if (v.date && v.date_len)
        copy_to_user((uint64_t)(uintptr_t)v.date, date, v.date_len);
    if (v.desc && v.desc_len)
        copy_to_user((uint64_t)(uintptr_t)v.desc, desc, v.desc_len);
    return copy_to_user(arg, &v, sizeof v);
}

int32_t drm_ioctl_get_unique(uint64_t arg)
{
    drm_unique_t u;
    if (copy_from_user(&u, arg, sizeof u) < 0)
        return -E_FAULT;
    static const char uniq[] = "gnos-bochs";
    if (u.unique && u.unique_len)
        copy_to_user((uint64_t)(uintptr_t)u.unique, uniq,
                     u.unique_len < sizeof uniq ? u.unique_len : sizeof uniq);
    return copy_to_user(arg, &u, sizeof u);
}

int32_t drm_ioctl_get_cap(uint64_t arg)
{
    drm_get_cap_t c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -E_FAULT;
    switch (c.capability) {
    case DRM_CAP_DUMB_BUFFER:           c.value = 1; break;
    case DRM_CAP_DUMB_PREFERRED_DEPTH:  c.value = 24; break;
    case DRM_CAP_DUMB_PREFER_SHADOW:    c.value = 0; break;
    case DRM_CAP_TIMESTAMP_MONOTONIC:   c.value = 1; break;
    case DRM_CAP_ADDFB2_MODIFIERS:      c.value = 0; break; /* linear only */
    case DRM_CAP_PRIME:
        /* wlroots' check_drm_features refuses the backend unless PRIME
         * import is advertised.  Dumb buffers are imported through the
         * handle->map dance rather than dmabuf fds, but the capability is
         * what the stack demands, so grant both directions. */
        c.value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
        break;
    case DRM_CAP_CRTC_IN_VBLANK_EVENT:
        /* wlroots requires vblank events for its page-flip completion
         * tracking.  GNOS synthesises DRM_EVENT_FLIP_COMPLETE on the
         * (synchronous) flip ioctl, which satisfies the same contract. */
        c.value = 1;
        break;
    default:                            c.value = 0; break;
    }
    return copy_to_user(arg, &c, sizeof c);
}

int32_t drm_ioctl_set_client_cap(uint64_t arg)
{
    drm_set_client_cap_t c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -E_FAULT;
    switch (c.capability) {
    case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
        /* The single primary plane is reported either way. */
        return 0;
    default:
        /* DRM_CLIENT_CAP_ATOMIC in particular: the modeset engine here is
         * legacy-only.  Accepting it would make wlroots drive a MODE_ATOMIC
         * commit path that does not exist.  Refuse loudly instead. */
        return -E_OPNOTSUPP;
    }
}

int32_t drm_ioctl_get_magic(uint64_t arg)
{
    static uint32_t next_magic = 0x1000;
    drm_auth_t a = { next_magic++ };
    return copy_to_user(arg, &a, sizeof a);
}

int32_t drm_ioctl_auth_magic(uint64_t arg)
{
    (void)arg;
    /* The magic-number dance is how libdrm's drmIsMaster() and wlroots'
     * allocator verify DRM master.  GNOS has no master concept: every
     * opener is master, so accept any token. */
    return 0;
}

int32_t drm_ioctl_set_master(uint64_t arg)
{
    (void)arg;
    /* Master acquisition is a no-op: GNOS has no master concept, every
     * opener is master. */
    return 0;
}

int32_t drm_ioctl_drop_master(uint64_t arg)
{
    (void)arg;
    return 0;
}
