/*
 * drm_init.c — device registration: publish /dev/dri/card0 and
 * /dev/dri/renderD128 and probe the bochs VBE_DISPI registers. (GPLv2)
 */
#include "drm_internal.h"
#include "subsys.h"
#include "debugcon.h"

const vfs_ops_t g_drm_ops = {
    .ioctl = drm_ioctl,
    .mmap  = drm_mmap,
    .read  = drm_read,
    .poll  = drm_poll,
};

int drm_init(const bootinfo_t *bi)
{
    g_cur_w = bi->fb_width;
    g_cur_h = bi->fb_height;
    g_cur_fb = 0;
    if (!g_cur_w || !g_cur_h) {
        dbg_puts("DRM: no boot framebuffer, staying out of the way\n");
        return 0;
    }

    /* Is the bochs VBE_DISPI register pair there?  (stdvga / bochs-display) */
    uint16_t id = vbe_read(VBE_ID);
    if (id >= 0xB0C0 && id <= 0xB0C5) {
        g_vbe = 1;
        dbg_puts("DRM: bochs VBE_DISPI present (id ");
        dbg_puts_hexn(id, 4);
        dbg_puts(")\r\n");
    } else {
        dbg_puts("DRM: no VBE_DISPI; only the boot mode is available\n");
    }

    int slot = subsys_register("drm", "card0", SUBSYS_CLASS_GRAPHIC, 29, 1);
    if (vfs_register_devnum("dri/card0", &g_drm_ops, NULL, 226, 0) != 0 ||
        vfs_register_devnum("dri/renderD128", &g_drm_ops, NULL, 226, 128) != 0) {
        subsys_set_state(slot, SUBSYS_STATE_FAILED);
        dbg_puts("DRM: device registration failed\n");
        return 0;
    }
    subsys_set_state(slot, SUBSYS_STATE_LIVE);

    uint32_t idx = 0;
    if (boot_mode_known(&idx))
        dbg_puts("DRM: /dev/dri/card0 ready (boot mode ");
    else
        dbg_puts("DRM: /dev/dri/card0 ready (boot mode ");
    dbg_puts_dec(g_cur_w);
    dbg_puts("x");
    dbg_puts_dec(g_cur_h);
    dbg_puts(")\r\n");
    return 1;
}
