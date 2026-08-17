/* ptrace.c -- ptrace(2) adapted to GNOS's process model.
 *
 * The one structural simplification over Linux: the tracer must be the
 * tracee's parent.  TRACEME always is (a child makes its parent its
 * tracer), and ATTACH refuses anything that is not a child, so the entire
 * mechanism rides on the waitpid/zombie machinery that already exists:
 *
 *   - a ptrace stop is an ordinary PROC_STOPPED whose stop_sig the tracer
 *     is told about through the existing wait status encoding
 *     (0x7F | (stop_sig << 8)), reported unconditionally -- waitpid needs
 *     no WUNTRACED for a traced child, exactly like Linux;
 *   - while stopped, a regs_t snapshot sits in p->ptrace_regs, so
 *     GETREGS/SETREGS read and write the stop image without ever touching
 *     the live kernel stack frame, and PTRACE_CONT replays it on resume;
 *   - a traced process's exit is an ordinary zombie: the tracer (== parent)
 *     reaps it with a normal WIFEXITED status, no WIFSTOPPED dance needed.
 *
 * Signal plumbing also follows Linux as far as a cooperative kernel can:
 * SIGKILL always kills, SIGCONT cannot resume a ptrace stop (only
 * PTRACE_CONT/SYSCALL/DETACH can), and whatever signal the tracer hands
 * back in the `data` argument of CONT is what gets delivered on resume.
 * PTRACE_SINGLESTEP is refused with EIO: a single-step needs the #DB
 * handler to arm TF and catch the trap, and this kernel's IDT treats #DB
 * like any other fault.  Memory peek/poke walks the tracee's page tables
 * (vmm_resolve) -- safe because a stopped tracee's address space is
 * frozen -- and needs no copy_from_user at all.
 */
#include <stdint.h>

#include "kstring.h"
#include "proc.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"
#include "ptrace.h"

/* ---- Linux register-image conversion --------------------------------- */

static void regs_to_user(ptrace_user_regs_t *u, const regs_t *r,
                         uint64_t orig_rax)
{
    memset(u, 0, sizeof *u);
    u->r15 = r->r15;  u->r14 = r->r14;  u->r13 = r->r13;  u->r12 = r->r12;
    u->rbp = r->rbp;  u->rbx = r->rbx;  u->r11 = r->r11;  u->r10 = r->r10;
    u->r9  = r->r9;   u->r8  = r->r8;
    u->rax = r->rax;  u->rcx = r->rcx;  u->rdx = r->rdx;
    u->rsi = r->rsi;  u->rdi = r->rdi;
    u->orig_rax = orig_rax;
    u->rip = r->rip;  u->cs = r->cs;    u->eflags = r->rflags;
    u->rsp = r->rsp;  u->ss = r->ss;
}

static void regs_from_user(regs_t *r, const ptrace_user_regs_t *u)
{
    r->r15 = u->r15;  r->r14 = u->r14;  r->r13 = u->r13;  r->r12 = u->r12;
    r->rbp = u->rbp;  r->rbx = u->rbx;  r->r11 = u->r11;  r->r10 = u->r10;
    r->r9  = u->r9;   r->r8  = u->r8;
    r->rax = u->rax;  r->rcx = u->rcx;  r->rdx = u->rdx;
    r->rsi = u->rsi;  r->rdi = u->rdi;
    r->rip = u->rip;  r->rflags = u->eflags;  r->rsp = u->rsp;
    /* cs/ss belong to the trap, not the program: the caller gets to keep
     * its own ring-3 selectors. */
}

/* ---- the stop itself -------------------------------------------------- */

/*
 * Suspend `p` until its tracer resumes it.  Mirrors stop_current() -- the
 * parent is woken out of its waitpid() the same way a job-control stop
 * wakes the shell -- but marks ptrace_stopped so that only a ptrace resume
 * (never SIGCONT) can unpark it, and keeps a register snapshot that the
 * tracer can rewrite with SETREGS.  On the way back out, the snapshot (as
 * the tracer left it) is replayed into the live trap frame.
 */
void ptrace_stop(proc_t *p, regs_t *r, int sig)
{
    p->ptrace_stopped = 1;
    p->ptrace_syscall_phase = 0;
    p->stop_sig = sig;
    p->reported = 0;

    regs_to_user((ptrace_user_regs_t *)p->ptrace_regs, r,
                 (uint64_t)p->syscall_nr);
    p->ptrace_regs_valid = 1;

    proc_t *tracer = proc_by_pid(p->tracer_pid);
    if (tracer) {
        proc_signal(tracer, SIGCHLD);
        if (tracer->state == PROC_BLOCKED &&
            tracer->wait_reason == WAIT_CHILD)
            sched_wake(tracer);
    }

    dbg_puts("PTRACE: pid ");
    dbg_puts_dec((uint32_t)p->pid);
    dbg_puts(" stopped (sig ");
    dbg_puts_dec((uint32_t)(sig & 0xFF));
    dbg_puts(")\r\n");

    /* The tracee parks as PROC_BLOCKED-on-WAIT_CHILD -- the same waiting
     * state waitpid() uses -- and ptrace_stopped marks it as a *ptrace*
     * stop.  It is deliberately not a PROC_STOPPED job-control stop:
     * nothing else may resume it (SIGCONT must not), and the marker is
     * what ptrace_resume() and the tracer's waitpid() look for. */
    p->wait_reason = WAIT_CHILD;
    sched_block(WAIT_CHILD);

    if (p->ptrace_regs_valid) {
        regs_from_user(r, (const ptrace_user_regs_t *)p->ptrace_regs);
        p->ptrace_regs_valid = 0;
    }
}

/* Hand the signal the tracer injected with CONT to check_signals: queue it
 * into the pending mask, where the delivery loop will pick it up on the way
 * back to user mode. */
static void ptrace_inject_resume_sig(proc_t *p)
{
    if (p->ptrace_resume_sig > 0 && p->ptrace_resume_sig < NSIG)
        p->sig_pending |= SIGMASK(p->ptrace_resume_sig);
    p->ptrace_resume_sig = 0;
    p->ptrace_stopped = 0;
}

/* Called from proc_check_signals() once a deliverable signal has been
 * picked (and cleared from the pending mask): the tracee stops with that
 * signal, and the tracer decides what actually happens to it. */
void ptrace_signal_stop(proc_t *p, regs_t *r, int sig)
{
    ptrace_stop(p, r, sig);
    ptrace_inject_resume_sig(p);
}

/* ---- syscall entry/exit stops ---------------------------------------- */

void ptrace_syscall_enter(regs_t *r, uint64_t nr)
{
    proc_t *p = proc_current();
    if (!p || !p->traced || p->ptrace_mode != PTRACE_RUN_SYSCALL)
        return;

    int sig = SIGTRAP;
    if (p->ptrace_opts & PTRACE_O_TRACESYSGOOD)
        sig |= 0x80;

    p->ptrace_syscall_phase = 1;
    ptrace_stop(p, r, sig);
    ptrace_inject_resume_sig(p);

    /* The tracer may have rewritten the register image; the handler must
     * still dispatch the call the tracee actually made. */
    r->rax = nr;
}

void ptrace_syscall_exit(regs_t *r)
{
    proc_t *p = proc_current();
    if (!p || !p->traced || p->ptrace_mode != PTRACE_RUN_SYSCALL ||
        p->ptrace_syscall_phase != 1)
        return;

    int sig = SIGTRAP;
    if (p->ptrace_opts & PTRACE_O_TRACESYSGOOD)
        sig |= 0x80;

    ptrace_stop(p, r, sig);
    ptrace_inject_resume_sig(p);
}

/* ---- memory access through the tracee's page tables ------------------ */

/*
 * PEEKDATA/PEEKTEXT read the tracee's memory word by word, walking *its*
 * page tables with vmm_resolve().  The tracee is stopped, so nothing moves
 * under us, and vmm_resolve returns the physical address of every user
 * page that exists -- a faulting address simply reads as EIO.
 */
static int ptrace_read_mem(proc_t *t, uint64_t addr, void *buf, uint64_t n)
{
    uint8_t *b = buf;

    if (!t->as)
        return -E_IO;

    while (n) {
        uint64_t phys = vmm_resolve(t->as, addr);
        if (!phys)
            return -E_IO;
        uint64_t chunk = PAGE_SIZE - (addr & (PAGE_SIZE - 1));
        if (chunk > n)
            chunk = n;
        memcpy(b, pmm_virt(phys), chunk);
        addr += chunk;
        b    += chunk;
        n    -= chunk;
    }
    return 0;
}

static int ptrace_write_mem(proc_t *t, uint64_t addr, const void *buf,
                            uint64_t n)
{
    if (!t->as)
        return -E_IO;
    /* vmm_copy_to_user already walks per page and refuses unmapped ones. */
    return vmm_copy_to_user(t->as, addr, buf, n) ? 0 : -E_IO;
}

/* ---- attach/detach ---------------------------------------------------- */

/*
 * ATTACH to a child: mark it traced and send SIGSTOP.  The stop is not
 * taken inside the syscall -- it lands in proc_check_signals() on the
 * child's way back to user mode, which is where a ptrace stop has to
 * happen anyway, because that is the one place a register image is real.
 */
static int ptrace_attach(proc_t *self, proc_t *t)
{
    if (t->ppid != self->pid)
        return -E_PERM;               /* GNOS: tracer must be the parent */
    if (t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
        return -E_SRCH;
    if (t->traced)
        return -E_PERM;

    t->traced           = 1;
    t->tracer_pid       = self->pid;
    t->ptrace_opts      = 0;
    t->ptrace_mode      = PTRACE_RUN_CONT;
    t->ptrace_stopped   = 0;
    t->ptrace_regs_valid = 0;

    proc_signal(t, SIGSTOP);
    return 0;
}

static int ptrace_resume(proc_t *t, int mode, int sig)
{
    if (!t->ptrace_stopped)
        return -E_SRCH;
    if (sig < 0 || sig >= NSIG)
        return -E_IO;

    t->ptrace_mode = mode;
    t->ptrace_resume_sig = sig;
    t->ptrace_stopped = 0;
    t->reported = 0;
    /* The tracee is parked in sched_block() inside ptrace_stop(); waking
     * it out of that wait is what unpins it from the stop. */
    if (t->state == PROC_BLOCKED && t->wait_reason == WAIT_CHILD)
        sched_wake(t);
    return 0;
}

/* ---- the syscall ------------------------------------------------------ */

int sys_ptrace(int request, int64_t pid, uint64_t addr, uint64_t data)
{
    proc_t *self = proc_current();
    if (!self)
        return -E_SRCH;

    if (request == PTRACE_TRACEME) {
        if (self->traced)
            return -E_PERM;
        self->traced           = 1;
        self->tracer_pid       = self->ppid;
        self->ptrace_opts      = 0;
        self->ptrace_mode      = PTRACE_RUN_CONT;
        self->ptrace_stopped   = 0;
        self->ptrace_regs_valid = 0;
        return 0;
    }

    proc_t *t = proc_by_pid((int)pid);
    if (!t || t->state == PROC_UNUSED)
        return -E_SRCH;

    if (request == PTRACE_ATTACH)
        return ptrace_attach(self, t);

    /* Every other request addresses the tracee's tracer. */
    if (t->tracer_pid != self->pid)
        return -E_SRCH;

    switch (request) {
    case PTRACE_CONT:
        return ptrace_resume(t, PTRACE_RUN_CONT, (int)data);

    case PTRACE_SYSCALL:
        return ptrace_resume(t, PTRACE_RUN_SYSCALL, (int)data);

    case PTRACE_KILL:
        /* Resume and let SIGKILL do the killing: it bypasses the ptrace
         * stop (see proc_check_signals), so the tracee dies immediately. */
        t->ptrace_stopped = 0;
        if (t->state == PROC_BLOCKED && t->wait_reason == WAIT_CHILD)
            sched_wake(t);
        proc_signal(t, SIGKILL);
        return 0;

    case PTRACE_DETACH: {
        int rc = ptrace_resume(t, PTRACE_RUN_CONT, (int)data);
        if (rc)
            return rc;
        t->traced     = 0;
        t->tracer_pid = 0;
        t->ptrace_opts = 0;
        return 0;
    }

    case PTRACE_SETOPTIONS:
        /* Only TRACESYSGOOD has an effect here; the rest of the option
         * space is accepted and ignored, which keeps gdb and strace happy
         * while they probe with TRACEEXEC/EXITKILL etc. */
        t->ptrace_opts = (int)data;
        return 0;

    case PTRACE_GETREGS: {
        ptrace_user_regs_t u;
        if (!t->ptrace_regs_valid)
            return -E_IO;             /* not stopped */
        if (!user_ptr_ok(data, sizeof u))
            return -E_FAULT;
        /* The snapshot already is Linux layout; no conversion needed. */
        memcpy(&u, t->ptrace_regs, sizeof u);
        memcpy((void *)(uintptr_t)data, &u, sizeof u);
        return 0;
    }

    case PTRACE_SETREGS: {
        ptrace_user_regs_t u;
        if (!t->ptrace_regs_valid)
            return -E_IO;
        if (!user_ptr_ok(data, sizeof u))
            return -E_FAULT;
        memcpy(&u, (const void *)(uintptr_t)data, sizeof u);
        memcpy(t->ptrace_regs, &u, sizeof u);
        return 0;
    }

    case PTRACE_GETREGSET: {
        /* Linux: ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov) with an
         * iovec { base, len } the kernel updates to what it actually
         * wrote. */
        struct { void *base; uint64_t len; } iov;
        if (!user_ptr_ok(addr, sizeof iov))
            return -E_FAULT;
        memcpy(&iov, (void *)(uintptr_t)addr, sizeof iov);
        if (!t->ptrace_regs_valid)
            return -E_IO;
        ptrace_user_regs_t u;
        uint64_t n = iov.len < sizeof u ? iov.len : sizeof u;
        if (n) {
            if (!user_ptr_ok((uint64_t)iov.base, n))
                return -E_FAULT;
            memcpy(iov.base, t->ptrace_regs, n);
        }
        iov.len = n;
        memcpy((void *)(uintptr_t)addr, &iov, sizeof iov);
        return 0;
    }

    case PTRACE_SETREGSET: {
        struct { const void *base; uint64_t len; } iov;
        if (!user_ptr_ok(addr, sizeof iov))
            return -E_FAULT;
        memcpy(&iov, (void *)(uintptr_t)addr, sizeof iov);
        if (!t->ptrace_regs_valid)
            return -E_IO;
        ptrace_user_regs_t u;
        uint64_t n = iov.len < sizeof u ? iov.len : sizeof u;
        if (n) {
            if (!user_ptr_ok((uint64_t)iov.base, n))
                return -E_FAULT;
            memcpy(&u, iov.base, n);
            memcpy(t->ptrace_regs, &u, n);
        }
        iov.len = n;
        memcpy((void *)(uintptr_t)addr, &iov, sizeof iov);
        return 0;
    }

    case PTRACE_PEEKDATA:
    case PTRACE_PEEKTEXT: {
        if (!t->ptrace_regs_valid)
            return -E_IO;             /* not stopped */
        uint64_t word;
        if (ptrace_read_mem(t, addr, &word, sizeof word) < 0)
            return -E_IO;
        if (!user_ptr_ok(data, sizeof word))
            return -E_FAULT;
        *(uint64_t *)(uintptr_t)data = word;
        return 0;
    }

    case PTRACE_POKEDATA:
    case PTRACE_POKETEXT: {
        if (!t->ptrace_regs_valid)
            return -E_IO;
        if (ptrace_write_mem(t, addr, &data, sizeof data) < 0)
            return -E_IO;
        return 0;
    }

    case PTRACE_GETSIGINFO: {
        /* No siginfo queue exists here, so synthesise the minimum the
         * Linux ABI promises: the signal number and SI_USER as the code.
         * si_signo/si_errno/si_code lead the struct in both libcs. */
        if (!t->ptrace_regs_valid)
            return -E_IO;
        if (!user_ptr_ok(data, 16))
            return -E_FAULT;
        int32_t *si = (int32_t *)(uintptr_t)data;
        si[0] = t->stop_sig & 0x7F;
        si[1] = 0;
        si[2] = 0;                    /* SI_USER */
        return 0;
    }

    default:
        /* SINGLESTEP and friends: not implemented, and loud about it. */
        return -E_IO;
    }
}