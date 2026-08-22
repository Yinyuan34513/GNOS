/*
 * drm_property.c — the property family: GETPROPERTY, OBJ_GETPROPERTIES,
 * SETPROPERTY, OBJ_SETPROPERTY, GETPROPBLOB and the (refused) ATOMIC. (GPLv2)
 *
 * The connector exposes one writable property (DPMS) and the plane one
 * immutable property (type); that is the minimum wlroots' legacy commit
 * path drives.  Atomic is deliberately not offered.
 */
#include "drm_internal.h"
#include "kstring.h"

static const uint32_t g_dpms_values[4] = { 0, 1, 2, 3 };
static const uint32_t g_type_values[3] = { 0, 1, 2 };

static int32_t drm_fill_property(drm_mode_get_property_t *p)
{
    switch (p->prop_id) {
    case DRM_PROP_ID_DPMS:              /* connector: enum, current On */
        p->flags = DRM_MODE_PROP_ENUM;
        memcpy(p->name, "DPMS", 5);
        p->count_values = 4;
        p->count_enum_blobs = 4;
        if (p->values_ptr && p->count_values >= 4)
            copy_to_user((uint64_t)(uintptr_t)p->values_ptr,
                         g_dpms_values, sizeof g_dpms_values);
        if (p->enum_blob_ptr && p->count_enum_blobs >= 4) {
            static const drm_mode_property_enum_t enums[4] = {
                { 0, "On" }, { 1, "Standby" }, { 2, "Suspend" }, { 3, "Off" },
            };
            copy_to_user((uint64_t)(uintptr_t)p->enum_blob_ptr,
                         enums, sizeof enums);
        }
        return 0;
    case DRM_PROP_ID_PLANE_TYPE:        /* plane: enum, value 1 = Primary */
        p->flags = DRM_MODE_PROP_ENUM;
        memcpy(p->name, "type", 5);
        p->count_values = 3;
        p->count_enum_blobs = 3;
        if (p->values_ptr && p->count_values >= 3)
            copy_to_user((uint64_t)(uintptr_t)p->values_ptr,
                         g_type_values, sizeof g_type_values);
        if (p->enum_blob_ptr && p->count_enum_blobs >= 3) {
            static const drm_mode_property_enum_t enums[3] = {
                { 0, "Overlay" }, { 1, "Primary" }, { 2, "Cursor" },
            };
            copy_to_user((uint64_t)(uintptr_t)p->enum_blob_ptr,
                         enums, sizeof enums);
        }
        return 0;
    case DRM_PROP_ID_EDID:              /* connector: blob, value = blob id */
        p->flags = DRM_MODE_PROP_BLOB;
        memcpy(p->name, "EDID", 5);
        p->count_values = 1;
        p->count_enum_blobs = 0;
        if (p->values_ptr && p->count_values >= 1) {
            static const uint32_t v[1] = { DRM_BLOB_ID_EDID };
            copy_to_user((uint64_t)(uintptr_t)p->values_ptr, v, sizeof v);
        }
        return 0;
    }
    return -E_NOENT;
}

int32_t drm_ioctl_get_property(uint64_t arg)
{
    drm_mode_get_property_t p;
    if (copy_from_user(&p, arg, sizeof p) < 0)
        return -E_FAULT;
    int32_t r = drm_fill_property(&p);
    if (r < 0)
        return r;
    return copy_to_user(arg, &p, sizeof p);
}

int32_t drm_ioctl_obj_get_properties(uint64_t arg)
{
    drm_mode_obj_get_properties_t o;
    if (copy_from_user(&o, arg, sizeof o) < 0)
        return -E_FAULT;

    static const uint32_t conn_props[2] = { DRM_PROP_ID_DPMS, DRM_PROP_ID_EDID };
    static const uint64_t conn_vals[2] = { 0, DRM_BLOB_ID_EDID };
    static const uint32_t plane_props[1] = { DRM_PROP_ID_PLANE_TYPE };
    static const uint64_t plane_vals[1] = { 1 };
    /* wlroots' get_drm_prop() asks with DRM_MODE_OBJECT_ANY: it wants a
     * specific property and filters the returned list by id itself.  With
     * one connector and one plane -- both object id 1 -- there is no way to
     * tell which kind was meant, so answer with the union.  A caller
     * looking for "type" finds it, one looking for "DPMS" finds it, and
     * anything else comes back missing, exactly as it should. */
    static const uint32_t any_props[3] = { DRM_PROP_ID_DPMS,
                                           DRM_PROP_ID_EDID,
                                           DRM_PROP_ID_PLANE_TYPE };
    static const uint64_t any_vals[3] = { 0, DRM_BLOB_ID_EDID, 1 };

    const uint32_t *props = NULL;
    const uint64_t *vals = NULL;
    uint32_t n = 0;

    switch (o.obj_type) {
    case DRM_MODE_OBJECT_ANY:
        if (o.obj_id != 1)
            return -E_NOENT;
        props = any_props; vals = any_vals; n = 2;
        break;
    case DRM_MODE_OBJECT_CONNECTOR:
        if (o.obj_id != 1)
            return -E_NOENT;
        props = conn_props; vals = conn_vals; n = 1;
        break;
    case DRM_MODE_OBJECT_PLANE:
        if (o.obj_id != 1)
            return -E_NOENT;
        props = plane_props; vals = plane_vals; n = 1;
        break;
    case DRM_MODE_OBJECT_CRTC:
        if (o.obj_id != 1)
            return -E_NOENT;
        break;                          /* no properties on the crtc */
    default:
        return -E_NOENT;
    }

    if (o.props_ptr && o.count_props >= n)
        copy_to_user((uint64_t)(uintptr_t)o.props_ptr, props, n * 4);
    if (o.prop_values_ptr && o.count_props >= n)
        copy_to_user((uint64_t)(uintptr_t)o.prop_values_ptr, vals, n * 8);
    o.count_props = n;
    return copy_to_user(arg, &o, sizeof o);
}

int32_t drm_ioctl_atomic(uint64_t arg)
{
    (void)arg;
    /* Not offered.  SET_CLIENT_CAP(DRM_CLIENT_CAP_ATOMIC) is refused, so a
     * well-behaved client never gets here; anything that does gets a loud
     * error instead of a silently partial modeset. */
    return -E_OPNOTSUPP;
}

/* drm_mode.h: connector property set.  wlroots' legacy commit turns DPMS on
 * before every SETCRTC, so the one property this driver exposes must be
 * writable. */
int32_t drm_ioctl_set_property(uint64_t arg)
{
    drm_mode_connector_set_property_t s;
    if (copy_from_user(&s, arg, sizeof s) < 0)
        return -E_FAULT;
    if (s.connector_id != 1 || s.prop_id != DRM_PROP_ID_DPMS)
        return -E_INVAL;
    /* DPMS value accepted and ignored: the panel is always on. */
    return 0;
}

/* drm_mode.h: object property set (atomic-style).  Nothing the legacy path
 * drives through this ioctl is state we keep, so accept any well-formed
 * write to a known object. */
int32_t drm_ioctl_obj_set_property(uint64_t arg)
{
    drm_mode_obj_set_property_t s;
    if (copy_from_user(&s, arg, sizeof s) < 0)
        return -E_FAULT;
    switch (s.obj_type) {
    case DRM_MODE_OBJECT_CRTC:
    case DRM_MODE_OBJECT_CONNECTOR:
    case DRM_MODE_OBJECT_PLANE:
        break;
    default:
        return -E_NOENT;
    }
    return 0;
}

/* drm_mode.h: property blob get.  The one blob this driver owns is the EDID
 * of its single virtual connector: a fixed 128-byte EDID 1.4 base block
 * (1280x800, "GNOS Display").  libdrm asks twice -- once for the length with
 * a NULL data pointer, then again for the bytes -- so serve both shapes. */
int32_t drm_ioctl_get_prop_blob(uint64_t arg)
{
    drm_mode_get_blob_t b;
    if (copy_from_user(&b, arg, sizeof b) < 0)
        return -E_FAULT;
    if (b.blob_id != DRM_BLOB_ID_EDID)
        return -E_NOENT;

    static const uint8_t g_edid[128] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x1d, 0xd8, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x22, 0x01, 0x04, 0x80, 0x28, 0x19, 0x78,
        0x02, 0x80, 0x4a, 0x2c, 0x58, 0x96, 0x3c, 0x38, 0x49, 0x96, 0x05, 0x00,
        0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x9e, 0x20, 0x00, 0x50, 0x50, 0x20,
        0x17, 0x30, 0x30, 0x20, 0x36, 0x4b, 0xcf, 0x10, 0x00, 0x00, 0x1e, 0x00,
        0x00, 0x00, 0x00, 0xfc, 0x47, 0x4e, 0x4f, 0x53, 0x20, 0x44, 0x69, 0x73,
        0x70, 0x6c, 0x61, 0x79, 0x20, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7b,
    };

    b.length = sizeof g_edid;
    if (b.data && b.length)
        copy_to_user((uint64_t)(uintptr_t)b.data, g_edid, sizeof g_edid);
    return copy_to_user(arg, &b, sizeof b);
}
