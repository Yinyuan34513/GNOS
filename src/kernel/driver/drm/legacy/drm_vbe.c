/*
 * drm_vbe.c — the bochs VBE_DISPI register interface and current scanout
 * state. (GPLv2)
 *
 * Modesetting goes through the Bochs VBE_DISPI registers (I/O 0x1CE/0x1CF),
 * the interface QEMU's stdvga and bochs-display have both implemented for
 * twenty-five years.  The framebuffer stays where it always was -- the
 * linear BAR0 the bootloader set up -- so fbcon keeps drawing into the same
 * memory and only its geometry changes.
 */
#include "drm_internal.h"
#include "io.h"

int g_vbe;                              /* VBE_DISPI answered the ID probe */

/* ---- current scanout state ------------------------------------------------ */
uint32_t g_cur_w, g_cur_h;              /* current mode */
uint32_t g_cur_fb;                      /* fb id on screen, 0 = console owns it */

void vbe_write(uint16_t idx, uint16_t val)
{
    outw(VBE_PORT_IDX, idx);
    outw(VBE_PORT_DAT, val);
}

uint16_t vbe_read(uint16_t idx)
{
    outw(VBE_PORT_IDX, idx);
    return inw(VBE_PORT_DAT);
}
