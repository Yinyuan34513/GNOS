/*
 * tty.h — the console terminal: framebuffer out, PS/2 keyboard in. (GPLv2)
 *
 * Registered with the VFS as /dev/tty, so user programs talk to it through
 * the ordinary read()/write() syscalls rather than a private interface.
 *
 * The tty is also where job control lives.  It remembers one *foreground*
 * process group; keyboard-generated signals go to that group, and a process
 * in any other group that tries to read gets SIGTTIN instead of stealing the
 * user's keystrokes.  That single piece of state is what lets a shell run
 * "count &" in the background and still own the keyboard itself.
 */
#ifndef GNUCOS_TTY_H
#define GNUCOS_TTY_H

#include <stdint.h>

#include "sysnum.h"        /* termios_t, winsize_t */

/* Hook up the keyboard IRQ and publish /dev/tty.  Call after idt_init(). */
void tty_init(void);

/* Kernel-side helpers (also used by the syscall layer). */
int32_t tty_write(const char *buf, uint32_t len);

/* Read according to the current line discipline: in canonical mode blocks
 * until a whole line is available, otherwise until VMIN bytes have arrived or
 * the VTIME timer runs out.  Returns 0 at end-of-file (Ctrl-D on an empty
 * line) and -E_INTR if a signal arrived. */
int32_t tty_read(char *buf, uint32_t len);

/* Foreground process group — the target of Ctrl-C, Ctrl-\ and Ctrl-Z. */
void tty_set_pgrp(int pgid);
int  tty_get_pgrp(void);

/*
 * POSIX gate for the terminal-*changing* operations.  A background process
 * that reconfigures the terminal is stopped with SIGTTOU; one that ignores or
 * blocks the signal is let through instead.  Returns 1 when the signal was
 * raised and the caller must fail with EINTR, 0 when it may go ahead.
 */
int  tty_check_ttou(void);

/* Line-discipline settings, behind TCGETS / TCSETS*.  `flush` corresponds to
 * TCSAFLUSH: throw away input that has not been read yet. */
void tty_get_termios(termios_t *t);
void tty_set_termios(const termios_t *t, int flush);

/* Console size in glyphs, behind TIOCGWINSZ. */
void tty_get_winsize(winsize_t *ws);

/* The VFS operations vector /dev/tty was registered with.  ioctl() compares
 * a descriptor's ops against this to decide whether it is the terminal. */
const void *tty_ops(void);

/* Non-zero when the keyboard ring buffer holds unread input (or an EOF mark),
 * i.e. a read from the tty would not block.  Used by poll(7)/ppoll(271). */
uint32_t tty_input_avail(void);

#endif
