/*
 * signal.c — the rt_sigframe: how a signal reaches a user handler. (GPLv2)
 *
 * Delivering a signal to a handler means running user code that the user code
 * being interrupted knows nothing about, and then putting everything back
 * exactly as it was.  The trick, which every Unix uses, is to do the saving on
 * the *user's* own stack:
 *
 *   1. push a frame holding the whole interrupted register set
 *   2. point RIP at the handler and RSP at the frame
 *   3. arrange for the handler's `ret` to land on a stub that calls
 *      rt_sigreturn(), which reads the frame back into the trap frame
 *
 * The frame layout here is byte-for-byte Linux's x86-64 rt_sigframe.  That is
 * not nostalgia: musl builds the third step for us.  Its sigaction() always
 * sets SA_RESTORER and points sa_restorer at its own __restore_rt, nine bytes
 * of `mov $15,%rax; syscall`, and its handlers read the ucontext at the
 * offsets Linux defines.  Inventing our own frame would mean patching libc.
 *
 * Two consequences of that compatibility are worth spelling out:
 *
 *   - musl's user-space ucontext_t is 936 bytes because it declares sigset_t
 *     as 128 bytes, while the kernel's is 304 with an 8-byte mask.  The
 *     siginfo we place just past the ucontext therefore lands *inside* what
 *     musl calls uc_sigmask.  That is exactly what happens on Linux too; only
 *     the first 8 bytes of uc_sigmask are ever meaningful.
 *
 *   - the frame goes below the 128-byte red zone.  GNOS's own programs are
 *     built with -mno-red-zone, but musl's libc.a and BusyBox are not, so
 *     leaf functions there really do keep live data below RSP.
 */
#include <stdint.h>

#include "signal.h"
#include "idt.h"        /* SYSCALL_VECTOR */
#include "gdt.h"        /* SEL_UCODE / SEL_UDATA */
#include "vmm.h"        /* USER_STACK_TOP / USER_STACK_SIZE / USER_LIMIT */
#include "vfs.h"        /* E_INTR */
#include "kstring.h"
#include "debugcon.h"

/* ---- the Linux x86-64 signal frame ------------------------------------- */
typedef struct {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
    uint64_t rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;               /* pointer to a 512-byte fxsave area */
    uint64_t reserved[8];
} sigcontext_t;

typedef struct {
    uint64_t ss_sp;
    int32_t  ss_flags;
    int32_t  __pad;
    uint64_t ss_size;
} stack_k_t;

typedef struct {
    uint64_t     uc_flags;
    uint64_t     uc_link;
    stack_k_t    uc_stack;
    sigcontext_t uc_mcontext;
    uint64_t     uc_sigmask;        /* the kernel's 8-byte sigset_t */
} ucontext_k_t;

typedef struct {
    int32_t  si_signo;
    int32_t  si_errno;
    int32_t  si_code;
    int32_t  __pad0;
    int32_t  si_pid;
    uint32_t si_uid;
    uint8_t  __pad[104];
} siginfo_k_t;

typedef struct {
    uint64_t     pretcode;          /* where the handler's `ret` goes */
    ucontext_k_t uc;
    siginfo_k_t  info;
} rt_sigframe_t;

/* Getting any of these wrong is a crash in user space with no clue attached,
 * so make it a build error instead. */
_Static_assert(sizeof(sigcontext_t)  == 256, "sigcontext must match Linux");
_Static_assert(sizeof(ucontext_k_t)  == 304, "ucontext must match Linux");
_Static_assert(sizeof(siginfo_k_t)   == 128, "siginfo must match Linux");
_Static_assert(sizeof(rt_sigframe_t) == 440, "rt_sigframe must match Linux");

#define SS_DISABLE  2

/* The FPU state fxsave writes, and MXCSR's position inside it. */
#define FPSTATE_SIZE   512
#define MXCSR_OFFSET   24
/* Reserved MXCSR bits fault on fxrstor.  A frame we are about to load came
 * from user memory, so it has to be filtered or a program can panic the
 * kernel by writing junk into its own signal frame. */
#define MXCSR_VALID    0x0000FFBFu

/*
 * RFLAGS bits a returning handler is allowed to choose.  Everything else --
 * IF, IOPL, NT, RF, TF, VM -- is the kernel's business: a frame is user
 * memory, and a program that could set IOPL or clear IF in it would own the
 * machine.
 */
#define RFLAGS_USER    0x00240CD5ull   /* CF PF AF ZF SF DF OF AC ID */
#define RFLAGS_FIXED   0x0000000000000202ull  /* bit 1 (always 1) + IF */

static int on_user_stack(uint64_t addr, uint64_t len)
{
    uint64_t lo = USER_STACK_TOP - USER_STACK_SIZE;
    if (addr < lo || addr >= USER_STACK_TOP)
        return 0;
    return addr + len <= USER_STACK_TOP;
}

/*
 * SA_RESTART.  A syscall that was cut short reports -EINTR in RAX; rewinding
 * RIP by the two bytes of the trap instruction and putting the call number
 * back makes it run again once the handler is done.  Both gates are two bytes
 * wide -- `int $0x80` is CD 80 and `syscall` is 0F 05 -- so one constant
 * covers the ulib and the musl paths alike.
 *
 * This has to happen *before* the frame is filled in, because the frame is
 * what rt_sigreturn resumes from: rewinding afterwards would restart the call
 * once and then still hand the handler's caller an EINTR.
 */
static void maybe_restart_syscall(proc_t *p, regs_t *r, int sig)
{
    if (r->vector != SYSCALL_VECTOR || p->syscall_nr < 0)
        return;
    if ((int64_t)r->rax != -E_INTR)
        return;
    if (!(p->sigact[sig].flags & SA_RESTART))
        return;

    r->rax  = (uint64_t)p->syscall_nr;
    r->rip -= 2;
}

int signal_deliver(proc_t *p, int sig, regs_t *r)
{
    const sigact_t *act = &p->sigact[sig];

    maybe_restart_syscall(p, r, sig);

    /* Carve out the fpstate first (it ends up at the higher address) and then
     * the frame, whose base must sit at 8 mod 16: that is where RSP is on
     * entry to a function reached by `call`, and compiled handlers issue
     * 16-byte-aligned SSE stores on that assumption. */
    uint64_t fp    = ((r->rsp - 128) - FPSTATE_SIZE) & ~63ull;
    uint64_t base  = ((fp - sizeof(rt_sigframe_t)) & ~15ull) - 8;

    if (!on_user_stack(base, sizeof(rt_sigframe_t)) ||
        !on_user_stack(fp, FPSTATE_SIZE))
        return -1;

    rt_sigframe_t *f = (rt_sigframe_t *)(uintptr_t)base;
    memset(f, 0, sizeof(*f));

    asm volatile("fxsave (%0)" :: "r"((uint8_t *)(uintptr_t)fp) : "memory");

    sigcontext_t *sc = &f->uc.uc_mcontext;
    sc->r8  = r->r8;  sc->r9  = r->r9;  sc->r10 = r->r10; sc->r11 = r->r11;
    sc->r12 = r->r12; sc->r13 = r->r13; sc->r14 = r->r14; sc->r15 = r->r15;
    sc->rdi = r->rdi; sc->rsi = r->rsi; sc->rbp = r->rbp; sc->rbx = r->rbx;
    sc->rdx = r->rdx; sc->rax = r->rax; sc->rcx = r->rcx; sc->rsp = r->rsp;
    sc->rip     = r->rip;
    sc->eflags  = r->rflags;
    sc->cs      = (uint16_t)r->cs;
    sc->ss      = (uint16_t)r->ss;
    sc->err     = r->errcode;
    sc->trapno  = r->vector;
    sc->oldmask = p->sig_mask;
    sc->fpstate = fp;

    f->uc.uc_stack.ss_flags = SS_DISABLE;   /* no sigaltstack support */
    f->uc.uc_sigmask        = p->sig_mask;

    /* We do not track who sent a signal, so every one of them looks like it
     * came from kill() by an unknown pid, which is what SI_USER means. */
    f->info.si_signo = sig;
    f->info.si_code  = SI_USER;

    f->pretcode = act->restorer;

    /* Block this signal (unless the handler asked not to) plus whatever the
     * handler nominated, for as long as it runs.  rt_sigreturn puts the old
     * set back from uc_sigmask. */
    p->sig_mask |= act->mask;
    if (!(act->flags & SA_NODEFER))
        p->sig_mask |= SIGMASK(sig);
    p->sig_mask &= ~(SIGMASK(SIGKILL) | SIGMASK(SIGSTOP));

    uint64_t handler = act->handler;
    if (act->flags & SA_RESETHAND)
        memset(&p->sigact[sig], 0, sizeof(p->sigact[sig]));

    /* Finally, the redirect.  The three arguments are always passed, with or
     * without SA_SIGINFO -- a plain one-argument handler just ignores RSI and
     * RDX -- and RAX is zeroed because the SysV varargs rule wants the number
     * of vector registers in AL. */
    r->rip = handler;
    r->rsp = base;
    r->rdi = (uint64_t)sig;
    r->rsi = (uint64_t)(uintptr_t)&f->info;
    r->rdx = (uint64_t)(uintptr_t)&f->uc;
    r->rax = 0;
    r->rflags &= ~(0x400ull | 0x100ull);    /* DF and TF off, as on Linux */

    return 0;
}

void signal_return(regs_t *r)
{
    proc_t *p = proc_current();
    if (!p)
        return;

    /* The restorer stub is reached by `ret`, which has already popped
     * pretcode, so the frame starts one word below the current RSP. */
    uint64_t base = r->rsp - 8;
    if (!on_user_stack(base, sizeof(rt_sigframe_t))) {
        dbg_puts("SIG: bad sigreturn frame, killing pid ");
        dbg_puts_dec((uint32_t)p->pid);
        dbg_puts("\r\n");
        p->term_sig = SIGSEGV;
        proc_exit(0);
        return;
    }

    const rt_sigframe_t *f  = (const rt_sigframe_t *)(uintptr_t)base;
    const sigcontext_t  *sc = &f->uc.uc_mcontext;

    r->r8  = sc->r8;  r->r9  = sc->r9;  r->r10 = sc->r10; r->r11 = sc->r11;
    r->r12 = sc->r12; r->r13 = sc->r13; r->r14 = sc->r14; r->r15 = sc->r15;
    r->rdi = sc->rdi; r->rsi = sc->rsi; r->rbp = sc->rbp; r->rbx = sc->rbx;
    r->rdx = sc->rdx; r->rax = sc->rax; r->rcx = sc->rcx;

    /*
     * Everything above is the user's own business.  Everything below decides
     * what privilege the process resumes at, so none of it is taken on trust:
     * the selectors are forced back to ring 3, RFLAGS keeps only the bits a
     * program may legitimately choose, and RIP/RSP have to be user addresses.
     */
    r->rip    = sc->rip;
    r->rsp    = sc->rsp;
    r->cs     = SEL_UCODE;
    r->ss     = SEL_UDATA;
    r->rflags = (sc->eflags & RFLAGS_USER) | RFLAGS_FIXED;

    if (r->rip >= USER_LIMIT || r->rsp >= USER_LIMIT) {
        dbg_puts("SIG: sigreturn to a non-user address, killing pid ");
        dbg_puts_dec((uint32_t)p->pid);
        dbg_puts("\r\n");
        p->term_sig = SIGSEGV;
        proc_exit(0);
        return;
    }

    p->sig_mask = f->uc.uc_sigmask & ~(SIGMASK(SIGKILL) | SIGMASK(SIGSTOP));

    uint64_t fp = sc->fpstate;
    if (fp && (fp & 15) == 0 && on_user_stack(fp, FPSTATE_SIZE)) {
        uint8_t *area = (uint8_t *)(uintptr_t)fp;
        uint32_t mxcsr;
        memcpy(&mxcsr, area + MXCSR_OFFSET, sizeof(mxcsr));
        mxcsr &= MXCSR_VALID;
        memcpy(area + MXCSR_OFFSET, &mxcsr, sizeof(mxcsr));
        asm volatile("fxrstor (%0)" :: "r"(area) : "memory");
    }

    /* This trap is not a syscall that anything could want restarted, and the
     * RAX we just restored is the interrupted code's, not a return value --
     * without this, a signal arriving now could mistake it for -EINTR. */
    p->syscall_nr = -1;
}
