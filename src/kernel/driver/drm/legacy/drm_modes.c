/*
 * drm_modes.c — the mode list and the modeinfo helpers that turn a mode_t
 * into the drm_mode_modeinfo_t a client's GETCONNECTOR/GETCRTC expects.
 * (GPLv2)
 *
 * The boot mode (whatever Limine set up, 1280x800 under QEMU) is mode 0 and
 * what GETCRTC reports until the first SETCRTC.  The rest are standard VGA
 * timings; the htotal/vtotal blanks are the CEA-ish defaults, close enough
 * for a virtual display that no monitor ever measures.
 */
#include "drm_internal.h"
#include "kstring.h"

const mode_t g_modes[N_MODES] = {
    { "1280x800", 1280, 800 },
    { "1024x768", 1024, 768 },
    { "800x600",  800,  600 },
    { "640x480",  640,  480 },
};

int boot_mode_known(uint32_t *index)
{
    for (int i = 0; i < N_MODES; i++)
        if (g_modes[i].h == g_cur_w && g_modes[i].v == g_cur_h) {
            *index = (uint32_t)i;
            return 1;
        }
    return 0;
}

void fill_modeinfo(drm_mode_modeinfo_t *m, const mode_t *src)
{
    memset(m, 0, sizeof(*m));
    m->clock      = (uint32_t)((uint64_t)src->h * src->v * 60 / 1000);
    m->hdisplay   = (uint16_t)src->h;
    m->hsync_start = (uint16_t)(src->h + 48);
    m->hsync_end   = (uint16_t)(src->h + 112);
    m->htotal      = (uint16_t)(src->h + 160);
    m->vdisplay    = (uint16_t)src->v;
    m->vsync_start = (uint16_t)(src->v + 3);
    m->vsync_end   = (uint16_t)(src->v + 6);
    m->vtotal      = (uint16_t)(src->v + 29);
    m->vrefresh    = 60;
    m->flags       = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
    for (int i = 0; src->name[i] && i < 31; i++)
        m->name[i] = src->name[i];
}

const mode_t *mode_by_size(uint32_t w, uint32_t h)
{
    for (int i = 0; i < N_MODES; i++)
        if (g_modes[i].h == w && g_modes[i].v == h)
            return &g_modes[i];
    return NULL;
}

void current_modeinfo(drm_mode_modeinfo_t *m)
{
    const mode_t *src = mode_by_size(g_cur_w, g_cur_h);
    if (src) {
        fill_modeinfo(m, src);
    } else {
        /* Boot set a mode not in our list: report it honestly. */
        memset(m, 0, sizeof(*m));
        m->clock    = (uint32_t)((uint64_t)g_cur_w * g_cur_h * 60 / 1000);
        m->hdisplay = (uint16_t)g_cur_w;
        m->vdisplay = (uint16_t)g_cur_h;
        m->htotal   = (uint16_t)(g_cur_w + 160);
        m->vtotal   = (uint16_t)(g_cur_h + 29);
        m->vrefresh = 60;
        m->flags    = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
    }
}
