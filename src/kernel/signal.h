/*
 * signal.h — delivering a signal to a user-installed handler. (GPLv2)
 */
#ifndef GNUCOS_SIGNAL_H
#define GNUCOS_SIGNAL_H

#include <stdint.h>

#include "panic.h"      /* regs_t */
#include "proc.h"

/*
 * Redirect `r` into p's handler for `sig`, leaving a Linux-compatible
 * rt_sigframe on the user stack for rt_sigreturn() to unwind.  Also applies
 * SA_RESTART, SA_NODEFER and SA_RESETHAND, and adds the handler's mask to the
 * blocked set.
 *
 * Returns 0 on success.  The one failure is "the frame does not fit on the
 * user stack", which on Linux is likewise fatal: the caller kills the process
 * with SIGSEGV, because there is nowhere left to tell it about the signal.
 */
int signal_deliver(proc_t *p, int sig, regs_t *r);

/* The rt_sigreturn() side: restore `r` (and the blocked set, and the FPU)
 * from the frame the handler is standing on.  Nothing is returned to the
 * caller -- every register including RAX comes from the frame. */
void signal_return(regs_t *r);

#endif
