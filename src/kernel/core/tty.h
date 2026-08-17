/*
 * tty.h — the virtual terminals: framebuffer out, PS/2 keyboard in. (GPLv2)
 *
 * There are NR_VT of them.  Each has its own line discipline, input ring,
 * termios, foreground process group and screen; exactly one is on the
 * framebuffer at a time and receives the keyboard.  Ctrl-Alt-F1..F6 -- or
 * the VT_ACTIVATE ioctl -- decides which.
 *
 * They are published as /dev/tty1 .. /dev/tty6.  /dev/console is always
 * terminal 1, where the kernel talks.  /dev/tty is not a terminal of its own
 * but a name for "whichever one is my controlling terminal", resolved per
 * caller out of proc_t::ctty.
 *
 * The tty is also where job control lives.  Each terminal remembers one
 * *foreground* process group; keyboard-generated signals go to that group,
 * and a process in any other group that tries to read gets SIGTTIN instead of
 * stealing the user's keystrokes.  That single piece of state is what lets a
 * shell run "count &" in the background and still own the keyboard itself --
 * and having one copy per terminal is what lets two shells on two terminals
 * do it independently.
 */
#ifndef GNUCOS_TTY_H
#define GNUCOS_TTY_H

#include <stdint.h>

#include "sysnum.h"        /* termios_t, winsize_t */

/* How many virtual terminals exist.  Six, matching the function keys the
 * switch is bound to and the getty lines init starts. */
#define NR_VT 6

struct vfs_node;

/* Hook up the keyboard IRQ and publish /dev/tty*.  Call after idt_init(). */
void tty_init(void);

/* ---- kernel-side helpers, on the caller's controlling terminal --------- */
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

/* The VFS operations vector every /dev/tty* was registered with.  ioctl()
 * compares a descriptor's ops against this to decide whether it is a
 * terminal. */
const void *tty_ops(void);

/* Non-zero when the ring buffer of the caller's controlling terminal holds
 * unread input (or an EOF mark), i.e. a read would not block. */
uint32_t tty_input_avail(void);

/*
 * The same question asked of one specific terminal, named by the VFS node a
 * descriptor was opened on.  poll(7) needs this form: a process may be
 * polling /dev/tty4 while sitting on tty1, and the global answer would be
 * about the wrong keyboard buffer.  A NULL node means /dev/tty.
 */
uint32_t tty_node_input_avail(const struct vfs_node *n);

/*
 * Push one decoded keystroke (a character after scancode translation) into
 * the *active* terminal's line discipline.  Shared by the keyboard IRQ and
 * the ttyinject(404) syscall so a headless test can simulate the keyboard.
 */
void tty_input_char(uint8_t c);

/* Feed a whole buffer of keystrokes through tty_input_char (syscall side). */
void tty_inject(const char *buf, uint32_t len);

/* ---- virtual terminal switching ---------------------------------------- */
/* Put terminal `n` (0-based) on the framebuffer and give it the keyboard.
 * Out-of-range indices are ignored. */
void tty_vt_switch(int n);

/* The terminal currently on screen, 0-based. */
int  tty_vt_active(void);

/* Release a terminal's claim on a session, called when a session leader
 * exits so the next getty can take it over. */
void tty_vt_release_session(int sid);

#endif
