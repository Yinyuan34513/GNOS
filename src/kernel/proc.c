/*
 * proc.c — processes, round-robin scheduling, fork/exec/wait and signals.
 * (GPLv2)
 *
 * The whole thing is deliberately small: a fixed process table, a fixed
 * kernel stack per slot, and a scheduler that walks the table looking for the
 * next runnable entry.  There is no run queue and no priority; with a handful
 * of processes the linear scan is cheaper than the bookkeeping would be.
 *
 * Kernel stacks live in the kernel's BSS.  That matters more than it looks:
 * the BSS is in the upper half, which every address space shares, so a task
 * can be interrupted, switched away from and resumed no matter whose page
 * tables happen to be loaded.
 */
#include <stddef.h>
#include <stdint.h>

#include "proc.h"
#include "signal.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"
#include "loader.h"
#include "gdt.h"
#include "timer.h"
#include "panic.h"
#include "kstring.h"
#include "debugcon.h"

/* switch.asm */
extern void switch_context(uint64_t *save_rsp, uint64_t load_rsp);
extern void ret_to_user(void);

/* Argument-vector limits.  These are the real ceiling on what a shell can
 * pass to a program, and BusyBox routinely receives more than sixteen words
 * (a glob expanding to a directory's worth of names is the usual way).  The
 * 4 KiB string arena lives on the 16 KiB kernel stack during execve, which is
 * comfortable but not somewhere to keep growing. */
#define MAX_ARGS      64
#define ARG_BYTES     4096
#define IMAGE_MAX     (4 * 1024 * 1024)

static proc_t  g_procs[MAX_PROCS];
static uint8_t g_kstacks[MAX_PROCS][KSTACK_SIZE] __attribute__((aligned(16)));

static proc_t *g_current;
static int     g_next_pid = 1;

/* The scheduler's own context: where switch_context parks the boot thread
 * (and where it returns to when there is nothing runnable at all). */
static uint64_t g_sched_rsp;

/* Scratch buffer for reading executables.  One at a time, in kernel BSS. */
static uint8_t g_image[IMAGE_MAX];

proc_t *proc_current(void) { return g_current; }

proc_t *proc_by_pid(int pid)
{
    for (int i = 0; i < MAX_PROCS; i++)
        if (g_procs[i].state != PROC_UNUSED && g_procs[i].pid == pid)
            return &g_procs[i];
    return NULL;
}

/* One clean FPU/SSE state, captured once at boot (see fpu_init_once) and
 * copied into every new process during proc_alloc, so each starts with the
 * default MXCSR and zeroed XMM registers.  Declared here, ahead of proc_alloc
 * which seeds it, and defined/initialised below. */
static uint8_t g_fpu_init[512] __attribute__((aligned(16)));

static proc_t *proc_alloc(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        if (g_procs[i].state != PROC_UNUSED)
            continue;

        proc_t *p = &g_procs[i];
        memset(p, 0, sizeof(*p));
        p->pid        = g_next_pid++;
        p->kstack_top = (uint64_t)(uintptr_t)g_kstacks[i] + KSTACK_SIZE;
        for (int f = 0; f < PROC_MAX_FD; f++)
            p->fds[f] = -1;
        /* Per-process memory bookkeeping musl expects the kernel to keep.
         * brk starts at the base of the user heap region; TLS and the futex
         * clear-word are zero until a program sets them. */
        p->sig_mask       = 0;
        p->syscall_nr     = -1;
        p->brk            = USER_BRK_BASE;
        p->fs_base        = 0;
        p->clear_child_tid = 0;
        p->nmmaps         = 0;
        /* Defaults for a process with no parent to inherit from; fork()
         * overwrites both from the parent a moment later. */
        strncpy(p->cwd, "/", GNUOS_PATH_MAX - 1);
        p->umask          = 022;
        /* Hand the new process the one clean FPU/SSE state captured at boot,
         * so it starts with the default MXCSR and zeroed XMM registers. */
        memcpy(p->fpu, g_fpu_init, sizeof(p->fpu));
        return p;
    }
    return NULL;
}

/* One clean FPU/SSE state, captured once at boot, copied into every new
 * process so it starts with the default MXCSR and zeroed XMM registers. */
static void fpu_init_once(void)
{
    asm volatile("fninit");
    asm volatile("fxsave (%0)" :: "r"(g_fpu_init) : "memory");
}

static void fpu_save(uint8_t *area)
{
    asm volatile("fxsave (%0)" :: "r"(area) : "memory");
}

static void fpu_load(uint8_t *area)
{
    asm volatile("fxrstor (%0)" :: "r"(area) : "memory");
}

void proc_init(void)
{
    memset(g_procs, 0, sizeof(g_procs));
    g_current = NULL;
    fpu_init_once();
}

/* Write the current process's TLS base into the FS base MSR.  Called by
 * schedule() on every context switch, and again from arch_prctl's handler, so
 * a process always runs with the %fs base it last asked for -- whether that
 * change happened between switches or mid-syscall.
 *
 * The user-mode FS segment base lives in IA32_FS_BASE (0xC0000100).  Do not
 * confuse it with IA32_KERNEL_GS_BASE (0xC0000102): writing the TLS there
 * swaps the *kernel* GS base and leaves user %fs pointing at 0, which makes
 * every %fs-relative TLS access by libc land in low memory and corrupts the
 * stack -- exactly the failure that crashed musl's hello at a builtin_tls
 * address. */
#define MSR_IA32_FS_BASE 0xC0000100ULL

static void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

void proc_set_fs(proc_t *p)
{
    if (!p)
        return;
    wrmsr((uint32_t)MSR_IA32_FS_BASE, p->fs_base);
}

/* ---- building a startable stack --------------------------------------- */
/*
 * Fabricate the kernel stack of a task that has "just been switched away
 * from" while sitting in ret_to_user with a full trap frame ready to go.
 * Returns the value to put in p->saved_rsp.
 */
static uint64_t build_startup_stack(proc_t *p, const regs_t *frame)
{
    uint64_t sp = p->kstack_top;

    /* 1. the trap frame ret_to_user will pop */
    sp -= sizeof(regs_t);
    regs_t *f = (regs_t *)(uintptr_t)sp;
    *f = *frame;

    /* 2. the return address switch_context will "return" to */
    sp -= 8;
    *(uint64_t *)(uintptr_t)sp = (uint64_t)(uintptr_t)ret_to_user;

    /* 3. the six callee-saved registers switch_context restores */
    sp -= 6 * 8;
    memset((void *)(uintptr_t)sp, 0, 6 * 8);

    return sp;
}

static void make_user_frame(regs_t *f, uint64_t entry, uint64_t stack)
{
    memset(f, 0, sizeof(*f));
    f->rip    = entry;
    f->cs     = SEL_UCODE;
    f->rflags = 0x202;                 /* IF set, nothing else */
    f->rsp    = stack;
    f->ss     = SEL_UDATA;
}

/* ---- argv ------------------------------------------------------------- */
/*
 * Lay out argc/argv at the top of the user stack the way a SysV _start
 * expects to find them:  [argc][argv0..argvN][NULL][envp NULL][auxv][AT_NULL].
 * Returns the new stack pointer, or 0 on failure.
 */
/* Push one auxv (type, value) pair.  The vector is built downwards, so the
 * value word lands above its type word and a bottom-up reader sees the pair
 * in the right order. */
static int push_aux(addrspace_t *as, uint64_t *sp, uint64_t type, uint64_t val)
{
    *sp -= 8;
    if (!vmm_copy_to_user(as, *sp, &val, 8))
        return 0;
    *sp -= 8;
    if (!vmm_copy_to_user(as, *sp, &type, 8))
        return 0;
    return 1;
}

static uint64_t push_args(addrspace_t *as, uint64_t stack_top,
                          char *const argv[], int argc, uint64_t phdr,
                          uint16_t phnum, uint64_t entry)
{
    uint64_t str_ptr[MAX_ARGS];
    uint64_t sp = stack_top;

    /* Strings first, from the very top downwards. */
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        sp -= len;
        sp &= ~7ULL;
        if (!vmm_copy_to_user(as, sp, argv[i], len))
            return 0;
        str_ptr[i] = sp;
    }

    /* auxv, written high-to-low like everything else, so a bottom-up reader
     * sees AT_PHDR first and AT_NULL last.
     *
     * AT_PHDR/AT_PHENT/AT_PHNUM are published only when the loader actually
     * located the program header table inside a mapped segment.  Advertising
     * AT_PHNUM with a NULL AT_PHDR is worse than saying nothing at all: musl's
     * __init_tls walks that many entries unconditionally and faults on the
     * first one.  With the triple absent it falls back to its builtin TLS. */
    sp &= ~15ULL;
    if (!push_aux(as, &sp, AT_NULL, 0))          return 0;
    if (!push_aux(as, &sp, AT_ENTRY, entry))     return 0;
    if (!push_aux(as, &sp, AT_PAGESZ, 4096))     return 0;
    if (phnum) {
        if (!push_aux(as, &sp, AT_PHNUM, phnum))                return 0;
        if (!push_aux(as, &sp, AT_PHENT, ELF64_PHDR_SIZE))      return 0;
        if (!push_aux(as, &sp, AT_PHDR, phdr))                  return 0;
    }

    /* envp terminator, then the argv array, then argc.
     *
     * argc, argv[0..argc-1], argv[argc]==NULL and envp[0]==NULL are one
     * contiguous block of argc+3 words, so any alignment padding has to go
     * *above* it -- a pad inserted anywhere inside would punch a hole in the
     * argv array.  The SysV ABI wants %rsp 16-byte aligned at process entry
     * (pointing at argc); that is Linux's STACK_ROUND in create_elf_tables,
     * and it is not the same as the call-site convention where %rsp%16==8
     * because a return address has been pushed. */
    sp &= ~15ULL;
    if (((sp - 8 * (uint64_t)(argc + 3)) & 15) != 0)
        sp -= 8;

    sp -= 8;
    uint64_t nul = 0;
    if (!vmm_copy_to_user(as, sp, &nul, 8))
        return 0;                                  /* envp[0] == NULL */

    sp -= 8;
    if (!vmm_copy_to_user(as, sp, &nul, 8))
        return 0;                                  /* argv[argc] == NULL */

    for (int i = argc - 1; i >= 0; i--) {
        sp -= 8;
        if (!vmm_copy_to_user(as, sp, &str_ptr[i], 8))
            return 0;
    }

    sp -= 8;
    uint64_t n = (uint64_t)argc;
    if (!vmm_copy_to_user(as, sp, &n, 8))
        return 0;

    return sp;
}

/* ---- image loading ---------------------------------------------------- */
/*
 * Read `path`, build a fresh address space around it and work out where its
 * stack pointer and instruction pointer should start.  Nothing about the
 * caller is touched, so a failure here is always recoverable.
 */
static int build_image(const char *path, char *const argv[], int argc,
                       addrspace_t **out_as, uint64_t *out_entry,
                       uint64_t *out_sp)
{
    uint32_t size = 0;
    if (!vfs_read_all(path, g_image, sizeof(g_image), &size))
        return -E_NOENT;

    addrspace_t *as = vmm_create();
    if (!as)
        return -E_NOMEM;

    uint64_t entry = 0;
    uint64_t phdr = 0;
    uint16_t phnum = 0;
    if (load_executable(as, g_image, size, &entry, &phdr, &phnum) <= 0) {
        vmm_destroy(as);
        return -E_INVAL;
    }

    if (!vmm_alloc_range(as, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE,
                         VM_USER | VM_WRITE)) {
        vmm_destroy(as);
        return -E_NOMEM;
    }

    uint64_t sp = push_args(as, USER_STACK_TOP - 16, argv, argc,
                            phdr, phnum, entry);
    if (!sp) {
        vmm_destroy(as);
        return -E_NOMEM;
    }

    *out_as    = as;
    *out_entry = entry;
    *out_sp    = sp;
    return 0;
}

int proc_spawn_init(const char *path)
{
    char *argv[1];
    argv[0] = (char *)path;

    addrspace_t *as;
    uint64_t entry, sp;
    int r = build_image(path, argv, 1, &as, &entry, &sp);
    if (r < 0)
        return r;

    proc_t *p = proc_alloc();
    if (!p) {
        vmm_destroy(as);
        return -E_NOMEM;
    }

    p->ppid = 0;
    p->pgid = p->pid;
    p->as   = as;
    strncpy(p->name, "init", sizeof(p->name) - 1);

    /* PID 1 is handed the console on fds 0/1/2; everything descended from it
     * inherits them through fork(), which is how a child shell ends up
     * talking to the same terminal without opening anything itself. */
    int h = vfs_file_open("/dev/tty", O_RDWR);
    if (h >= 0) {
        p->fds[0] = h;
        p->fds[1] = h; vfs_file_ref(h);
        p->fds[2] = h; vfs_file_ref(h);
    }

    regs_t f;
    make_user_frame(&f, entry, sp);
    p->saved_rsp = build_startup_stack(p, &f);
    p->state     = PROC_READY;

    dbg_puts("PROC: init is pid ");
    dbg_puts_dec((uint32_t)p->pid);
    dbg_puts(", entry ");
    dbg_puts_hex(entry);
    dbg_puts("\r\n");
    return p->pid;
}

/* ---- fork ------------------------------------------------------------- */
int proc_fork(regs_t *r)
{
    proc_t *parent = g_current;

    proc_t *child = proc_alloc();
    if (!child)
        return -E_NOMEM;

    child->as = vmm_clone(parent->as);
    if (!child->as) {
        child->state = PROC_UNUSED;
        return -E_NOMEM;
    }

    child->ppid        = parent->pid;
    child->pgid        = parent->pgid;
    child->sig_ignored = parent->sig_ignored;
    /* The child inherits the parent's whole memory profile: blocked
     * signals, the break, the TLS base and every anonymous mapping, so a
     * post-fork execve starts from a clean copy and a post-fork return to
     * libc sees the same heap it left. */
    child->sig_mask        = parent->sig_mask;
    /* Handlers survive fork -- the address space is a copy, so the handler
     * addresses still point at the same code.  (exec is where they have to
     * go away; see proc_execve.) */
    memcpy(child->sigact, parent->sigact, sizeof(child->sigact));
    child->brk             = parent->brk;
    child->fs_base         = parent->fs_base;
    child->clear_child_tid = parent->clear_child_tid;
    child->nmmaps          = parent->nmmaps;
    for (int m = 0; m < parent->nmmaps; m++)
        child->mmaps[m] = parent->mmaps[m];
    /* The current directory and the file-creation mask are inherited too;
     * that is what lets a shell `cd` once and have every command it forks
     * start there. */
    strncpy(child->cwd, parent->cwd, GNUOS_PATH_MAX - 1);
    child->umask           = parent->umask;
    strncpy(child->name, parent->name, sizeof(child->name) - 1);

    /* Shared file offsets are the point of the reference count. */
    for (int i = 0; i < PROC_MAX_FD; i++) {
        child->fds[i] = parent->fds[i];
        if (child->fds[i] >= 0)
            vfs_file_ref(child->fds[i]);
    }

    /* The child resumes exactly where the parent's syscall will return,
     * except that fork() reports 0 to it. */
    regs_t f = *r;
    f.rax = 0;
    child->saved_rsp = build_startup_stack(child, &f);
    child->state     = PROC_READY;

    return child->pid;
}

/* ---- execve ----------------------------------------------------------- */
int proc_execve(const char *path, char *const argv[], regs_t *r)
{
    proc_t *p = g_current;

    /* Copy the argument vector out of the old address space before we throw
     * it away; the strings live in the caller's memory. */
    char  argbuf[ARG_BYTES];
    char *args[MAX_ARGS];
    int   argc = 0;
    uint32_t used = 0;

    while (argc < MAX_ARGS && argv && argv[argc]) {
        const char *s = argv[argc];
        uint32_t len = (uint32_t)strlen(s) + 1;
        if (used + len > sizeof(argbuf))
            return -E_INVAL;
        memcpy(argbuf + used, s, len);
        args[argc] = argbuf + used;
        used += len;
        argc++;
    }
    if (argc == 0) {
        strncpy(argbuf, path, sizeof(argbuf) - 1);
        args[0] = argbuf;
        argc = 1;
    }

    char pathbuf[64];
    strncpy(pathbuf, path, sizeof(pathbuf) - 1);
    pathbuf[sizeof(pathbuf) - 1] = 0;

    addrspace_t *as;
    uint64_t entry, sp;
    int rc = build_image(pathbuf, args, argc, &as, &entry, &sp);
    if (rc < 0)
        return rc;

    /* Past this point the old image is gone and there is no way back. */
    addrspace_t *old = p->as;
    p->as = as;
    vmm_switch(as);
    vmm_destroy(old);

    /* A fresh image gets a fresh break and no TLS/mappings of its own. */
    p->brk             = USER_BRK_BASE;
    p->fs_base         = 0;
    p->clear_child_tid = 0;
    p->nmmaps          = 0;

    /*
     * Every caught signal goes back to its default action.  This is not a
     * nicety: a handler address points into the image we just destroyed, so
     * letting one survive means the next signal jumps into whatever the new
     * program happens to have at that address.  Blocked and ignored sets do
     * carry over, which is what POSIX says and what lets a shell hand a child
     * an inherited SIG_IGN.
     */
    memset(p->sigact, 0, sizeof(p->sigact));

    const char *base = pathbuf;
    for (const char *c = pathbuf; *c; c++)
        if (*c == '/')
            base = c + 1;
    strncpy(p->name, base, sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = 0;

    /* Rewrite the trap frame in place: when the syscall returns, it returns
     * into the new program instead of the old one. */
    make_user_frame(r, entry, sp);
    return 0;
}

/* ---- exit / wait ------------------------------------------------------ */
void proc_exit(int status)
{
    proc_t *p = g_current;

    for (int i = 0; i < PROC_MAX_FD; i++) {
        if (p->fds[i] >= 0) {
            vfs_file_unref(p->fds[i]);
            p->fds[i] = -1;
        }
    }

    /* Futex-based pthread_join relies on the kernel zeroing the thread's
     * tid word when it dies; musl set the address via set_tid_address and
     * waits on it.  Do this before we tear the address space down. */
    if (p->clear_child_tid && p->as) {
        uint64_t z = 0;
        vmm_copy_to_user(p->as, p->clear_child_tid, &z, 8);
    }

    if (p->as) {
        /* We are still running on this address space, so step off it first. */
        vmm_switch_kernel();
        vmm_destroy(p->as);
        p->as = NULL;
    }

    /* Orphans are adopted by init, which is the only process guaranteed to
     * still be around to reap them. */
    for (int i = 0; i < MAX_PROCS; i++) {
        if (g_procs[i].state != PROC_UNUSED && g_procs[i].ppid == p->pid)
            g_procs[i].ppid = 1;
    }

    p->exit_status = status;
    p->state       = PROC_ZOMBIE;

    proc_t *parent = proc_by_pid(p->ppid);
    if (parent) {
        proc_signal(parent, SIGCHLD);
        if (parent->state == PROC_BLOCKED && parent->wait_reason == WAIT_CHILD)
            sched_wake(parent);
    }

    sched_yield();
    panic("proc_exit: scheduled a zombie");
}

int proc_waitpid(int pid, int *status, int options)
{
    proc_t *p = g_current;

    for (;;) {
        int have_child = 0;

        for (int i = 0; i < MAX_PROCS; i++) {
            proc_t *c = &g_procs[i];
            if (c->state == PROC_UNUSED || c->ppid != p->pid)
                continue;
            if (pid > 0 && c->pid != pid)
                continue;
            have_child = 1;

            if (c->state == PROC_ZOMBIE) {
                int cpid = c->pid;
                if (status)
                    *status = c->term_sig ? (c->term_sig & 0x7F)
                                          : ((c->exit_status & 0xFF) << 8);
                c->state = PROC_UNUSED;
                return cpid;
            }

            /* WUNTRACED is how a shell learns that a job it started has been
             * suspended rather than having finished. */
            if ((options & WUNTRACED) && c->state == PROC_STOPPED &&
                !c->reported) {
                c->reported = 1;
                if (status)
                    *status = 0x7F | ((c->stop_sig & 0xFF) << 8);
                return c->pid;
            }
        }

        if (!have_child)
            return -E_CHILD;
        if (options & WNOHANG)
            return 0;

        p->wait_pid     = pid;
        p->wait_reason  = WAIT_CHILD;
        sched_block(WAIT_CHILD);

        /* A signal can break the sleep without a child having exited. */
        if (proc_pending_signals(p))
            return -E_INTR;
    }
}

/* ---- signals ---------------------------------------------------------- */
/* Default disposition of each signal. */
static int sig_is_stop(int s)
{
    return s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU;
}

static int sig_is_ignored_by_default(int s)
{
    return s == SIGCHLD;
}

/* Signals that must never abort a blocking system call. */
#define SIG_NOINTR  (SIGMASK(SIGCHLD) | SIGMASK(SIGCONT))

uint64_t proc_pending_signals(const proc_t *p)
{
    if (!p)
        return 0;
    /* A blocked signal must not break a sleeper out of its call: nobody is
     * going to deliver it on the way back, so the caller would just return
     * EINTR, be restarted, and hit the same still-pending bit forever. */
    return p->sig_pending & ~p->sig_ignored & ~p->sig_mask & ~SIG_NOINTR;
}

int proc_signal_blocked(const proc_t *p, int sig)
{
    if (!p || sig <= 0 || sig >= NSIG)
        return 0;
    return ((p->sig_ignored | p->sig_mask) & SIGMASK(sig)) != 0;
}

int proc_signal(proc_t *p, int sig)
{
    if (!p || sig <= 0 || sig >= NSIG || p->state == PROC_UNUSED ||
        p->state == PROC_ZOMBIE)
        return -E_INVAL;

    /* SIGCONT resumes immediately, before anyone looks at the pending set:
     * a stopped process is not running and would never get around to it. */
    if (sig == SIGCONT) {
        p->sig_pending &= ~(SIGMASK(SIGSTOP) | SIGMASK(SIGTSTP) |
                            SIGMASK(SIGTTIN) | SIGMASK(SIGTTOU));
        if (p->state == PROC_STOPPED) {
            p->reported = 0;
            p->state    = PROC_READY;
        }
        return 0;
    }

    /* An ignored signal is discarded outright rather than queued: leaving it
     * pending would mean a later signal(sig, SIG_DFL) suddenly delivered a
     * signal that was sent while the process was not listening. */
    if (p->sig_ignored & SIGMASK(sig))
        return 0;

    if (sig_is_stop(sig))
        p->sig_pending &= ~SIGMASK(SIGCONT);

    p->sig_pending |= SIGMASK(sig);

    /* Wake a sleeper so it can notice; SIGKILL/SIGSTOP even overrule a
     * stopped state. */
    if (p->state == PROC_BLOCKED)
        sched_wake(p);
    else if (p->state == PROC_STOPPED && sig == SIGKILL)
        p->state = PROC_READY;

    return 0;
}

int proc_signal_group(int pgid, int sig)
{
    int n = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *p = &g_procs[i];
        if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE)
            continue;
        if (p->pgid != pgid)
            continue;
        proc_signal(p, sig);
        n++;
    }
    return n;
}

static void stop_current(int sig)
{
    proc_t *p = g_current;

    p->sig_pending &= ~SIGMASK(sig);
    p->state    = PROC_STOPPED;
    p->stop_sig = sig;
    p->reported = 0;

    proc_t *parent = proc_by_pid(p->ppid);
    if (parent) {
        proc_signal(parent, SIGCHLD);
        if (parent->state == PROC_BLOCKED && parent->wait_reason == WAIT_CHILD)
            sched_wake(parent);
    }

    dbg_puts("PROC: pid ");
    dbg_puts_dec((uint32_t)p->pid);
    dbg_puts(" stopped\r\n");

    sched_yield();
}

/*
 * Called on the way back to user mode.  Each pending signal resolves either
 * to a user handler -- see signal.c for the frame that gets built -- or to
 * its default action: terminate, stop, or nothing at all.
 */
void proc_check_signals(regs_t *r)
{
    proc_t *p = g_current;
    if (!p)
        return;

    /* Only act when returning to ring 3; a signal taken in the middle of
     * kernel work would leave the kernel's own state inconsistent. */
    if ((r->cs & 3) != 3)
        return;

    for (;;) {
        uint64_t deliverable = p->sig_pending & ~p->sig_ignored & ~p->sig_mask;
        if (!deliverable)
            return;

        int sig = 0;
        for (int s = 1; s < NSIG; s++) {
            if (deliverable & SIGMASK(s)) {
                sig = s;
                break;
            }
        }
        if (!sig)
            return;

        p->sig_pending &= ~SIGMASK(sig);

        /*
         * A caught signal, and the only case that leaves this function early:
         * the trap frame now points at the handler, so there is no "rest of
         * the return path" left to deliver a second signal on.  Anything else
         * still pending gets its turn when the handler's rt_sigreturn comes
         * back through here.
         */
        uint64_t h = p->sigact[sig].handler;
        if (h != SIG_DFL && h != SIG_IGN && sig != SIGKILL && sig != SIGSTOP) {
            if (signal_deliver(p, sig, r) == 0)
                return;
            /* No room on the user stack for the frame.  There is no way to
             * tell the process about the signal, so it dies of the attempt --
             * which is what Linux does too. */
            dbg_puts("PROC: pid ");
            dbg_puts_dec((uint32_t)p->pid);
            dbg_puts(" has no stack for a signal frame\r\n");
            p->term_sig = SIGSEGV;
            proc_exit(0);
        }

        if (sig_is_ignored_by_default(sig) || sig == SIGCONT)
            continue;

        if (sig_is_stop(sig)) {
            stop_current(sig);
            continue;                    /* resumed by SIGCONT; look again */
        }

        dbg_puts("PROC: pid ");
        dbg_puts_dec((uint32_t)p->pid);
        dbg_puts(" killed by signal ");
        dbg_puts_dec((uint32_t)sig);
        dbg_puts("\r\n");
        p->term_sig = sig;
        proc_exit(0);
    }
}

/* ---- scheduler -------------------------------------------------------- */
static proc_t *pick_next(void)
{
    /* Start scanning after the current slot so nobody starves. */
    int start = 0;
    if (g_current)
        start = (int)(g_current - g_procs) + 1;

    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *p = &g_procs[(start + i) % MAX_PROCS];
        if (p->state == PROC_READY)
            return p;
    }
    return NULL;
}

/*
 * Hand the CPU to the next runnable process.  Called both voluntarily
 * (sched_yield) and from the timer interrupt.
 */
static void schedule(void)
{
    proc_t *prev = g_current;
    proc_t *next = pick_next();

    /* Park the outgoing task's FPU/SSE registers.  We must do this whenever
     * we leave a real process -- including one that voluntarily blocked
     * (sched_block() sets it PROC_BLOCKED *before* calling schedule()) -- so
     * its XMM state is not lost.  The idle thread has g_current == NULL and
     * owns no FPU state, so it is skipped.  FXSAVE copies the live FPU into
     * the buffer without disturbing the live state, so returning early
     * (prev == next) simply keeps running with the same registers. */
    if (prev)
        fpu_save(prev->fpu);

    if (!next) {
        /* Nothing else is runnable.  If the caller is still running it may
         * simply carry on -- bouncing to the idle thread here would park a
         * perfectly runnable task and hang the machine. */
        if (!prev || prev->state == PROC_RUNNING)
            return;
        g_current = NULL;
        vmm_switch_kernel();
        uint64_t *save = &prev->saved_rsp;
        switch_context(save, g_sched_rsp);
        return;
    }

    if (prev == next && prev->state == PROC_RUNNING)
        return;

    if (prev && prev->state == PROC_RUNNING)
        prev->state = PROC_READY;

    next->state = PROC_RUNNING;
    g_current   = next;

    /* A trap from ring 3 has to land on this process's kernel stack. */
    tss_set_rsp0(next->kstack_top);
    proc_set_fs(next);                 /* restore this task's TLS base */
    vmm_switch(next->as);
    fpu_load(next->fpu);               /* restore this task's XMM registers */

    uint64_t *save = prev ? &prev->saved_rsp : &g_sched_rsp;
    switch_context(save, next->saved_rsp);
}

void sched_yield(void)
{
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags));
    schedule();
    if (flags & 0x200)
        asm volatile("sti");
}

void sched_tick(void)
{
    /* Already inside an interrupt gate, so interrupts are off. */
    if (g_current)
        schedule();
}

void sched_block(wait_reason_t why)
{
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags));

    if (g_current) {
        g_current->state       = PROC_BLOCKED;
        g_current->wait_reason = why;
        schedule();
    }

    if (flags & 0x200)
        asm volatile("sti");
}

void sched_block_irqoff(wait_reason_t why)
{
    if (!g_current)
        return;
    g_current->state       = PROC_BLOCKED;
    g_current->wait_reason = why;
    schedule();
}

void sched_block_timeout(wait_reason_t why, uint64_t ticks)
{
    proc_t *p = g_current;
    if (!p)
        return;
    p->state       = PROC_BLOCKED;
    p->wait_reason = why;
    p->wake_tick   = timer_ticks() + (ticks ? ticks : 1);
    schedule();
    p->wake_tick   = 0;
}

void sched_wake(proc_t *p)
{
    if (p && p->state == PROC_BLOCKED) {
        p->wait_reason = WAIT_NONE;
        p->wake_tick   = 0;
        p->state       = PROC_READY;
    }
}

void sched_expire_timeouts(void)
{
    uint64_t now = timer_ticks();
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *p = &g_procs[i];
        if (p->state == PROC_BLOCKED && p->wake_tick && now >= p->wake_tick)
            sched_wake(p);
    }
}

void sched_wake_reason(wait_reason_t why)
{
    for (int i = 0; i < MAX_PROCS; i++)
        if (g_procs[i].state == PROC_BLOCKED && g_procs[i].wait_reason == why)
            sched_wake(&g_procs[i]);
}

/*
 * The boot thread becomes the idle loop.  It never dies: whenever every
 * process is blocked, schedule() comes back here and we halt until the next
 * interrupt makes somebody runnable again.
 */
void sched_start(void)
{
    dbg_puts("SCHED: entering the run loop\r\n");

    for (;;) {
        asm volatile("cli");
        if (pick_next()) {
            schedule();
            continue;
        }
        /* sti and hlt must be adjacent: sti only unmasks after the next
         * instruction, which closes the wake-up race. */
        asm volatile("sti; hlt");
    }
}
