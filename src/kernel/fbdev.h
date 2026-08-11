/*
 * fbdev.h — /dev/fb0, a Linux-compatible framebuffer device. (GPLv2)
 *
 * fbcon owns the framebuffer as a *text console*; fbdev exposes the very same
 * pixels to user space as a file.  The two coexist deliberately: the console
 * keeps working (that is where the boot log goes) while a program that opens
 * /dev/fb0 can mmap the scanlines and draw over them, exactly as it would on
 * Linux.  Nothing arbitrates between them, because nothing does on Linux
 * either -- last writer wins.
 *
 * The ioctl numbers and the struct layouts live in fbdev.c and are the ones
 * from <linux/fb.h>, byte for byte, so a program built against the real
 * header (BusyBox's fbset, SDL's fbcon backend) sees what it expects.  They
 * are not re-exported here: nothing inside the kernel needs them, and a
 * second, subtly different copy of those structs is exactly the kind of thing
 * that silently breaks a userland client.
 */
#ifndef GNUCOS_FBDEV_H
#define GNUCOS_FBDEV_H

#include <stdint.h>
#include "bootinfo.h"
#include "gfx.h"

/* Register /dev/fb0 over the bootloader's framebuffer.  Returns 1 on success,
 * 0 if the machine booted without a usable linear framebuffer. */
int fbdev_init(const bootinfo_t *bi);

/* A gfx surface over the live framebuffer, for in-kernel drawing.  The base
 * pointer is NULL when fbdev_init() failed, and every gfx primitive treats
 * that as "draw nothing", so callers need no separate check. */
gfx_surface_t fbdev_screen(void);

/* Non-fatal self check: draw into a corner of the real framebuffer, read the
 * pixels back through the same path user space would use, then restore what
 * was there.  Prints one line to the debug console. */
void fbdev_self_test(void);

#endif
