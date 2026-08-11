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

/*
 * Argument-vector limits.  These are the real ceiling on what a shell can
 * pass to a program, and BusyBox routinely receives more than sixteen words
 * (a glob expanding to a directory's worth of names is the usual way).
 *
 * The arena has to hold argv *and* envp now, and an interactive bash exports
 * a few kilobytes of environment before it has run a single command, so 4 KiB
 * no longer covers a plain `ls`.  16 KiB does -- but it can no longer live on
 * the 16 KiB kernel stack, so the arena and the two pointer vectors moved to
 * BSS below.  Sharing one static arena between callers is safe because the
 * kernel is never preempted: timer.c only calls sched_tick() when the trap
 * came from ring 3, so an execve runs to completion once it starts.
 */
#define MAX_ARGS      128
#define MAX_ENVS      128
#define ARG_BYTES     16384
#define IMAGE_MAX     (8 * 1024 * 1024)

/* How many `#!` lines deep execve will chase an interpreter before giving up
 * with ELOOP.  Linux uses 4; a script whose interpreter is a script whose
 * interpreter is a script is already pathological. */
#define EXEC_INTERP_MAX 4

static proc_t  g_procs[MAX_PROCS];
static uint8_t g_kstacks[MAX_PROCS][KSTACK_SIZE] __attribute__((aligned(16)));

/* execve's string arena and pointer vectors.  See the comment above for why
 * these are static rather than automatic. */
static char  g_argbuf[ARG_BYTES];
static uint32_t g_argused;
static char *g_args[MAX_ARGS + 1];
static char *g_envs[MAX_ENVS + 1];

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
        p->start_tick = timer_ticks();
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

/* ---- argv / envp ------------------------------------------------------ */
/*
 * Lay out the initial process stack the way a SysV _start expects to find it:
 *
 *   [argc][argv0..argvN][NULL][envp0..envpM][NULL][auxv pairs][AT_NULL]
 *   ... then the string bodies themselves, higher up ...
 *
 * Returns the new stack pointer, or 0 on failure.
 *
 * The environment used to be hardcoded to an empty vector here, which is a
 * subtle way to break every program that is not a toy: bash with no PATH
 * cannot find a single external command, and with no TERM readline falls back
 * to a dumb terminal.  envp is now carried end to end from the execve caller.
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

/* Pointer slots for the strings we push, filled top-down.  Static for the
 * same reason the arena is: MAX_ARGS + MAX_ENVS words is 2 KiB and the kernel
 * stack is only 16 KiB. */
static uint64_t g_strptr[MAX_ARGS + MAX_ENVS];

static uint64_t push_args(addrspace_t *as, uint64_t stack_top,
                          char *const argv[], int argc,
                          char *const envp[], int envc,
                          uint64_t phdr, uint16_t phnum, uint64_t entry)
{
    uint64_t *arg_ptr = g_strptr;
    uint64_t *env_ptr = g_strptr + MAX_ARGS;
    uint64_t sp = stack_top;

    /* Strings first, from the very top downwards: environment, then argv. */
    for (int i = envc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(envp[i]) + 1;
        sp -= len;
        sp &= ~7ULL;
        if (!vmm_copy_to_user(as, sp, envp[i], len))
            return 0;
        env_ptr[i] = sp;
    }
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        sp -= len;
        sp &= ~7ULL;
        if (!vmm_copy_to_user(as, sp, argv[i], len))
            return 0;
        arg_ptr[i] = sp;
    }

    /*
     * AT_RANDOM points at sixteen bytes the libc uses to seed its stack
     * guard.  musl copes with the entry being absent, but it then leaves the
     * canary zero; supplying real bytes costs one push and makes the
     * -fstack-protector builds of third-party userland meaningful.
     */
    uint64_t at_random = 0;
    {
        uint64_t seed[2];
        uint64_t t = timer_ticks() * 6364136223846793005ULL + 1442695040888963407ULL;
        seed[0] = t ^ (uint64_t)(uintptr_t)as;
        t = t * 6364136223846793005ULL + 1442695040888963407ULL;
        seed[1] = t ^ entry;
        sp -= 16;
        sp &= ~15ULL;
        if (!vmm_copy_to_user(as, sp, seed, 16))
            return 0;
        at_random = sp;
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
    if (!push_aux(as, &sp, AT_NULL, 0))            return 0;
    if (!push_aux(as, &sp, AT_ENTRY, entry))       return 0;
    if (!push_aux(as, &sp, AT_PAGESZ, 4096))       return 0;
    if (!push_aux(as, &sp, AT_CLKTCK, SCHED_HZ))   return 0;
    if (!push_aux(as, &sp, AT_SECURE, 0))          return 0;
    if (!push_aux(as, &sp, AT_RANDOM, at_random))  return 0;
    if (phnum) {
        if (!push_aux(as, &sp, AT_PHNUM, phnum))                return 0;
        if (!push_aux(as, &sp, AT_PHENT, ELF64_PHDR_SIZE))      return 0;
        if (!push_aux(as, &sp, AT_PHDR, phdr))                  return 0;
    }

    /* The envp array, then the argv array, then argc.
     *
     * argc, argv[0..argc-1], argv[argc]==NULL, envp[0..envc-1] and
     * envp[envc]==NULL are one contiguous block of argc+envc+3 words, so any
     * alignment padding has to go *above* it -- a pad inserted anywhere
     * inside would punch a hole in one of the two arrays.  The SysV ABI wants
     * %rsp 16-byte aligned at process entry (pointing at argc); that is
     * Linux's STACK_ROUND in create_elf_tables, and it is not the same as the
     * call-site convention where %rsp%16==8 because a return address has been
     * pushed. */
    sp &= ~15ULL;
    if (((sp - 8 * (uint64_t)(argc + envc + 3)) & 15) != 0)
        sp -= 8;

    sp -= 8;
    uint64_t nul = 0;
    if (!vmm_copy_to_user(as, sp, &nul, 8))
        return 0;                                  /* envp[envc] == NULL */

    for (int i = envc - 1; i >= 0; i--) {
        sp -= 8;
        if (!vmm_copy_to_user(as, sp, &env_ptr[i], 8))
            return 0;
    }

    sp -= 8;
    if (!vmm_copy_to_user(as, sp, &nul, 8))
        return 0;                                  /* argv[argc] == NULL */

    for (int i = argc - 1; i >= 0; i--) {
        sp -= 8;
        if (!vmm_copy_to_user(as, sp, &arg_ptr[i], 8))
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
                       char *const envp[], int envc,
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
    /* ENOEXEC, not EINVAL: "I read the file and it is not something I can
     * run" is a distinct answer from "your arguments were wrong", and bash
     * keys its fallback-to-shell-script behaviour off exactly this errno. */
    if (load_executable(as, g_image, size, &entry, &phdr, &phnum) <= 0) {
        vmm_destroy(as);
        return -E_NOEXEC;
    }

    if (!vmm_alloc_range(as, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE,
                         VM_USER | VM_WRITE)) {
        vmm_destroy(as);
        return -E_NOMEM;
    }

    uint64_t sp = push_args(as, USER_STACK_TOP - 16, argv, argc, envp, envc,
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

/*
 * Record argv in the NUL-separated form /proc/<pid>/cmdline is defined to
 * have.  Truncation is silent and safe: the buffer always ends on a complete
 * NUL-terminated word, so a reader parsing it can never run off the end.
 */
static void proc_set_cmdline(proc_t *p, char *const argv[], int argc)
{
    uint32_t n = 0;
    for (int i = 0; i < argc && argv[i]; i++) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        if (n + len > sizeof(p->cmdline))
            break;
        memcpy(p->cmdline + n, argv[i], len);
        n += len;
    }
    p->cmdline_len = n;
}

/*
 * The environment PID 1 is born with, and therefore -- since every other
 * process descends from it through fork -- the default environment of the
 * whole system.  Setting it here rather than in init.c means it is already in
 * place for /etc/rc, which runs before anything has a chance to export
 * anything.  PATH is what lets a shell find a command by name at all; TERM is
 * what stops readline from falling back to its dumb-terminal line editor.
 */
static char *const g_init_env[] = {
    (char *)"PATH=/bin:/usr/bin:/sbin:/usr/sbin",
    (char *)"HOME=/root",
    (char *)"TERM=linux",
    (char *)"SHELL=/bin/bash",
    (char *)"USER=root",
    (char *)"LOGNAME=root",
    (char *)"TMPDIR=/tmp",
    (char *)"PWD=/",
    NULL,
};
#define INIT_ENVC ((int)(sizeof(g_init_env) / sizeof(g_init_env[0])) - 1)

int proc_spawn_init(const char *path)
{
    char *argv[1];
    argv[0] = (char *)path;

    addrspace_t *as;
    uint64_t entry, sp;
    int r = build_image(path, argv, 1, g_init_env, INIT_ENVC,
                        &as, &entry, &sp);
    if (r < 0)
        return r;

    proc_t *p = proc_alloc();
    if (!p) {
        vmm_destroy(as);
        return -E_NOMEM;
    }

    p->ppid = 0;
    p->pgid = p->pid;
    /* PID 1 is the session leader of the one session that exists at boot, and
     * every process inherits that sid until something calls setsid(). */
    p->sid  = p->pid;
    p->as   = as;
    strncpy(p->name, "init", sizeof(p->name) - 1);
    proc_set_cmdline(p, &argv[0], 1);

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
    child->sid         = parent->sid;
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
    memcpy(child->cmdline, parent->cmdline, parent->cmdline_len);
    child->cmdline_len = parent->cmdline_len;

    /* Shared file offsets are the point of the reference count. */
    for (int i = 0; i < PROC_MAX_FD; i++) {
        child->fds[i] = parent->fds[i];
        if (child->fds[i] >= 0)
            vfs_file_ref(child->fds[i]);
    }
    /* fork copies the flag; only exec acts on it. */
    child->fd_cloexec = parent->fd_cloexec;

    /* The child resumes exactly where the parent's syscall will return,
     * except that fork() reports 0 to it. */
    regs_t f = *r;
    f.rax = 0;
    child->saved_rsp = build_startup_stack(child, &f);
    child->state     = PROC_READY;

    return child->pid;
}

/* ---- clone ------------------------------------------------------------
 * musl implements both fork() and pthread_create on top of the clone(2)
 * syscall, so a musl-linked userland (OpenRC, bash, ...) depends on it even
 * for a plain fork.  The toy has no copy-on-write and no shared-memory
 * threads worth the bookkeeping, so every case is handled as a full address
 * space copy -- exactly what proc_fork does.  That makes CLONE_VM "threads"
 * independent copies rather than truly shared, which is wrong if something
 * joins on a shared variable, but it cannot corrupt the parent's memory the
 * way sharing the page tables would when the child exits first.  The handful
 * of flags libc actually uses (TLS, the parent/child tid bookkeeping) are
 * honoured so futex-based synchronisation at least has a real tid to aim at.
 */
#define CLONE_VM           0x00000100
#define CLONE_SETTLS       0x00040000
#define CLONE_PARENT_SETTID 0x00080000
#define CLONE_CHILD_CLEARTID 0x00100000
#define CLONE_CHILD_SETTID 0x00200000

int proc_clone(regs_t *r)
{
    uint64_t  flags      = r->rdi;
    void     *child_stack = (void *)r->rsi;
    int      *parent_tid  = (int *)r->rdx;
    int      *child_tid   = (int *)r->r10;
    uintptr_t tls         = (uintptr_t)r->r8;

    proc_t *parent = g_current;

    proc_t *child = proc_alloc();
    if (!child)
        return -E_NOMEM;

    /* No COW: the child gets its own copy of the parent's address space. */
    child->as = vmm_clone(parent->as);
    if (!child->as) {
        child->state = PROC_UNUSED;
        return -E_NOMEM;
    }

    child->ppid        = parent->pid;
    child->pgid        = parent->pgid;
    child->sid         = parent->sid;
    child->sig_ignored = parent->sig_ignored;
    child->sig_mask    = parent->sig_mask;
    memcpy(child->sigact, parent->sigact, sizeof(child->sigact));
    child->brk         = parent->brk;
    child->fs_base     = parent->fs_base;
    if (flags & CLONE_SETTLS)
        child->fs_base = tls;
    /* A plain copy inherits the parent's clear_child_tid; clone lets the
     * caller set its own so a thread can be joined via FUTEX_WAIT on it. */
    child->clear_child_tid = (flags & CLONE_CHILD_CLEARTID)
                                 ? (uintptr_t)child_tid
                                 : parent->clear_child_tid;
    child->nmmaps      = parent->nmmaps;
    for (int m = 0; m < parent->nmmaps; m++)
        child->mmaps[m] = parent->mmaps[m];
    strncpy(child->cwd, parent->cwd, GNUOS_PATH_MAX - 1);
    child->umask       = parent->umask;
    strncpy(child->name, parent->name, sizeof(child->name) - 1);
    memcpy(child->cmdline, parent->cmdline, parent->cmdline_len);
    child->cmdline_len = parent->cmdline_len;

    for (int i = 0; i < PROC_MAX_FD; i++) {
        child->fds[i] = parent->fds[i];
        if (child->fds[i] >= 0)
            vfs_file_ref(child->fds[i]);
    }
    child->fd_cloexec = parent->fd_cloexec;

    /* CLONE_PARENT_SETTID is written into the *parent's* address space, and
     * it happens immediately; CLONE_CHILD_SETTID is written into the child's
     * once it starts (here, before it is marked runnable). */
    if (flags & CLONE_PARENT_SETTID) {
        int pid = child->pid;
        vmm_copy_to_user(parent->as, (uint64_t)parent_tid, &pid, sizeof pid);
    }
    if (flags & CLONE_CHILD_SETTID) {
        int pid = child->pid;
        vmm_copy_to_user(child->as, (uint64_t)child_tid, &pid, sizeof pid);
    }

    regs_t f = *r;
    f.rax = 0;
    /* A thread (child_stack != NULL) must run on the stack libc prepared
     * rather than the parent's copied one. */
    if (child_stack)
        f.rsp = (uint64_t)child_stack;
    child->saved_rsp = build_startup_stack(child, &f);
    child->state     = PROC_READY;

    return child->pid;
}

/* ---- execve ----------------------------------------------------------- */
/*
 * Copy one string into the shared arena and hand back a pointer to it.
 * Returns NULL when the arena is full, which the caller turns into E2BIG.
 */
static char *arena_dup(const char *s)
{
    uint32_t len = (uint32_t)strlen(s) + 1;
    if (g_argused + len > sizeof(g_argbuf))
        return NULL;
    char *dst = g_argbuf + g_argused;
    memcpy(dst, s, len);
    g_argused += len;
    return dst;
}

/*
 * Resolve one level of `#!` interpretation.
 *
 * Returns 1 if `path` was a script and the vectors were rewritten, 0 if it
 * was not a script (leave it alone and let the ELF loader have it), or a
 * negative errno.
 *
 * The parsing rules are Linux's, from fs/binfmt_script.c, and the details
 * matter for compatibility:
 *
 *   - only the first 256 bytes are examined, and a line longer than that with
 *     no newline in it is not a script at all;
 *   - everything after the interpreter path is ONE argument, not a word list.
 *     "#!/bin/sh -eu" passes the single string "-eu"; it does not pass "-e"
 *     and "-u".  Splitting it would break every script that relies on the
 *     single-argument rule, which is most of the ones that use it;
 *   - the original argv[0] is discarded and replaced by the interpreter, and
 *     the script's own path is spliced in as the interpreter's first file
 *     argument.
 *
 * The path we splice in is the already-normalised absolute one rather than
 * the string the caller passed, so the interpreter can still find the script
 * if it happens to chdir() before opening it.
 */
static int resolve_interp(char *pathbuf, char **args, int *argc)
{
    int h = vfs_file_open(pathbuf, O_RDONLY);
    if (h < 0)
        return 0;                       /* let build_image report the real error */

    char hdr[257];
    int32_t got = vfs_file_read(h, hdr, sizeof(hdr) - 1);
    vfs_file_unref(h);
    if (got < 2 || hdr[0] != '#' || hdr[1] != '!')
        return 0;
    hdr[got] = 0;

    /* Truncate at the first newline.  No newline in the first 256 bytes means
     * this is not a script header, whatever it looks like. */
    char *nl = hdr;
    while (*nl && *nl != '\n')
        nl++;
    if (*nl != '\n')
        return -E_NOEXEC;
    *nl = 0;

    char *s = hdr + 2;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s)
        return -E_NOEXEC;               /* "#!" with nothing after it */

    char *interp = s;
    while (*s && *s != ' ' && *s != '\t')
        s++;
    char *optarg = NULL;
    if (*s) {
        *s++ = 0;
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s) {
            optarg = s;
            /* Strip trailing whitespace so "#!/bin/sh -e   " does not pass an
             * argument with three spaces glued to the end of it. */
            char *e = optarg + strlen(optarg);
            while (e > optarg && (e[-1] == ' ' || e[-1] == '\t'))
                *--e = 0;
        }
    }

    int extra = optarg ? 2 : 1;
    if (*argc + extra > MAX_ARGS)
        return -E_2BIG;

    char *interp_c = arena_dup(interp);
    char *optarg_c = optarg ? arena_dup(optarg) : NULL;
    char *script_c = arena_dup(pathbuf);
    if (!interp_c || !script_c || (optarg && !optarg_c))
        return -E_2BIG;

    /* Shift the caller's arguments up, dropping the old argv[0], and write
     * the new head of the vector underneath them. */
    for (int i = *argc - 1; i >= 1; i--)
        args[i + extra] = args[i];
    args[0] = interp_c;
    if (optarg_c) {
        args[1] = optarg_c;
        args[2] = script_c;
    } else {
        args[1] = script_c;
    }
    *argc += extra;

    strncpy(pathbuf, interp_c, GNUOS_PATH_MAX - 1);
    pathbuf[GNUOS_PATH_MAX - 1] = 0;
    return 1;
}

int proc_execve(const char *path, char *const argv[], char *const envp[],
                regs_t *r)
{
    proc_t *p = g_current;

    /*
     * Copy both vectors out of the old address space before we throw it
     * away; the strings live in the caller's memory and every pointer in
     * them dies with it.  argv and envp share one arena, so a program with a
     * huge environment has correspondingly less room for arguments -- which
     * is exactly how Linux's ARG_MAX behaves too.
     */
    g_argused = 0;

    int argc = 0;
    while (argc < MAX_ARGS && argv && argv[argc]) {
        char *c = arena_dup(argv[argc]);
        if (!c)
            return -E_2BIG;
        g_args[argc++] = c;
    }
    if (argc == 0) {
        char *c = arena_dup(path);
        if (!c)
            return -E_2BIG;
        g_args[argc++] = c;
    }

    int envc = 0;
    while (envc < MAX_ENVS && envp && envp[envc]) {
        char *c = arena_dup(envp[envc]);
        if (!c)
            return -E_2BIG;
        g_envs[envc++] = c;
    }
    g_args[argc] = NULL;
    g_envs[envc] = NULL;

    char pathbuf[GNUOS_PATH_MAX];
    strncpy(pathbuf, path, sizeof(pathbuf) - 1);
    pathbuf[sizeof(pathbuf) - 1] = 0;

    /* Chase `#!` lines until we reach something the ELF loader can take. */
    for (int depth = 0; ; depth++) {
        if (depth >= EXEC_INTERP_MAX)
            return -E_LOOP;
        int got = resolve_interp(pathbuf, g_args, &argc);
        if (got < 0)
            return got;
        if (got == 0)
            break;
        g_args[argc] = NULL;
    }

    addrspace_t *as;
    uint64_t entry, sp;
    int rc = build_image(pathbuf, g_args, argc, g_envs, envc, &as, &entry, &sp);
    if (rc < 0)
        return rc;

    /* Past this point the old image is gone and there is no way back. */
    addrspace_t *old = p->as;
    p->as = as;
    vmm_switch(as);
    vmm_destroy(old);

    /*
     * Only now do the close-on-exec descriptors actually close.  Doing it
     * any earlier would be a bug: an exec that fails (missing file, bad
     * ELF) must leave the caller exactly as it was, and a shell that has
     * already closed the pipe it was about to report the error down has no
     * way to complain.
     */
    for (int i = 0; i < PROC_MAX_FD; i++) {
        if ((p->fd_cloexec & (1ULL << i)) && p->fds[i] >= 0) {
            vfs_file_unref(p->fds[i]);
            p->fds[i] = -1;
        }
    }
    p->fd_cloexec = 0;

    /* Record what we are running now, for /proc/[pid]/cmdline and ps. */
    proc_set_cmdline(p, g_args, argc);

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

        /*
         * A signal can break the sleep without a child having exited.
         * SIGCHLD is the exception, and it has to be: it is almost always
         * the very wakeup we were waiting for, so returning EINTR here
         * would mean wait() never once succeeded on a process that
         * installed a SIGCHLD handler.  Loop round and reap instead; the
         * handler still runs on the way back to user mode, and if nothing
         * turned out to be collectable we simply block again.
         */
        if (proc_pending_signals(p) & ~SIGMASK(SIGCHLD))
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

/*
 * Signals that must never abort a blocking system call.  SIGCONT is here
 * because by the time anyone inspects the pending set the process has
 * already been resumed -- the wakeup was the whole point of the signal, and
 * there is nothing further to report.
 */
#define SIG_NOINTR  (SIGMASK(SIGCONT))

/*
 * Does delivering this signal actually do anything?  A signal whose default
 * action is "ignore" and for which no handler is installed is dropped on the
 * way back to user mode, so letting it break a blocking call would hand the
 * caller an EINTR with nothing behind it: the program restarts the call,
 * finds the world unchanged, and the only trace is a mysterious short read.
 * Once a handler exists the signal does have an effect and POSIX wants the
 * call interrupted so that handler can run -- which is exactly what a shell
 * relies on to reap background jobs while sitting in read().
 */
static int sig_has_effect(const proc_t *p, int s)
{
    if (!sig_is_ignored_by_default(s))
        return 1;
    /* 0 is SIG_DFL and 1 is SIG_IGN; anything else is a real handler. */
    return p->sigact[s].handler > 1;
}

uint64_t proc_pending_signals(const proc_t *p)
{
    if (!p)
        return 0;
    /* A blocked signal must not break a sleeper out of its call: nobody is
     * going to deliver it on the way back, so the caller would just return
     * EINTR, be restarted, and hit the same still-pending bit forever. */
    uint64_t s = p->sig_pending & ~p->sig_ignored & ~p->sig_mask & ~SIG_NOINTR;

    for (int sig = 1; sig < NSIG; sig++)
        if ((s & SIGMASK(sig)) && !sig_has_effect(p, sig))
            s &= ~SIGMASK(sig);

    return s;
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

        /* ITIMER_REAL fires regardless of what state its owner is in, so it
         * is checked before the sleeper test rather than inside it.  Re-arm
         * from `now` and not from the old deadline: a periodic timer whose
         * process was starved should not then fire in a burst catching up. */
        if (p->itimer_expire && now >= p->itimer_expire) {
            p->itimer_expire = p->itimer_interval ? now + p->itimer_interval
                                                  : 0;
            proc_signal(p, SIGALRM);
        }

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
