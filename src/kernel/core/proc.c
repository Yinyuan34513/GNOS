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
#include "ptrace.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"
#include "loader.h"
#include "gdt.h"
#include "timer.h"
#include "panic.h"
#include "heap.h"
#include "kstring.h"
#include "debugcon.h"
#include "smp.h"

/* switch.asm */
extern void switch_context(uint64_t *save_rsp, uint64_t load_rsp);
extern void ret_to_user(void);
extern void kthread_trampoline(void);

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
/* Whole-image exec buffer.  The statically-linked GTK3 desktop binaries
 * (xfce4-panel, thunar, ...) are 18-20 MB each; 16 MB made every one of
 * them fail to exec with a confusing "not found" from the shell. */
#define IMAGE_MAX     (64 * 1024 * 1024)

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

/* The current process, one per core: schedule() parks the outgoing task's
 * context in its own saved_rsp and stores the incoming one here. */
#define g_current (cpu_self()->current)

static int     g_next_pid = 1;

/* ---- EEVDF run queue --------------------------------------------------
 * A binary min-heap of runnable processes keyed on virtual deadline,
 * guarded by g_proc_lock -- the one lock every core can contend for, so it
 * is held only for the few instructions of a heap step and never across a
 * context switch (see schedule()). */
static spinlock_t g_proc_lock;
static proc_t    *g_rq[MAX_PROCS];
static unsigned   g_rq_size;

/* The global virtual clock, in ticks.  Advances by the time a process
 * actually runs; an idle CPU advances nothing. */
static uint64_t g_vtime;

/* Default slice in ticks (SCHED_HZ = 100, so 50 ms per pick). */
#define EEVDF_SLICE_TICKS 5

/* spawn_init/fork/clone all finish a fresh process the same way; the
 * SIGCONT/SIGKILL paths enqueue too (enqueue_fresh under g_proc_lock). */
static void enqueue_fresh(proc_t *p);
static void rq_remove(proc_t *p);
static void proc_make_runnable(proc_t *p);

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
        p->tgid       = p->pid;      /* a fresh process is its own group leader */
        p->start_tick = timer_ticks();
        p->kstack_top = (uint64_t)(uintptr_t)g_kstacks[i] + KSTACK_SIZE;
        /* EEVDF: the slice is fixed; vruntime/deadline are set when the
         * process joins the run queue.  Fresh processes own no BKL and
         * run nowhere yet. */
        p->rq_index     = -1;
        p->bkl_held     = 0;
        p->on_cpu       = -1;
        p->slice        = EEVDF_SLICE_TICKS;
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
        /* Defaults for a process with no parent to inherit from; fork()
         * overwrites both from the parent a moment later. */
        strncpy(p->cwd, "/", GNUOS_PATH_MAX - 1);
        p->umask          = 022;
        /* No controlling terminal until something claims one with TIOCSCTTY;
         * PID 1 is given terminal 1 explicitly in proc_spawn_init(). */
        p->ctty           = -1;
        /* Credentials default to root.  Everything on the system descends
         * from PID 1, which is root, and fork() overwrites these from the
         * parent immediately -- so the default only ever applies to init. */
        p->uid = p->euid = p->suid = 0;
        p->gid = p->egid = p->sgid = 0;
        p->ngroups        = 0;
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
    uint32_t gl, gh;
    asm volatile("rdmsr" : "=a"(gl), "=d"(gh) : "c"((uint32_t)0xC0000101));
    dbg_puts("PROC: gsbase=");
    dbg_puts_hex(((uint64_t)gh << 32) | gl);
    dbg_puts("\r\n");
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
                          uint64_t phdr, uint16_t phnum, uint64_t entry,
                          uint64_t ldso_base)
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
    /*
     * Align the WHOLE vector block in one shot: everything from here down
     * to argc is (auxv pairs) + (argc+envc+3) words, and SysV wants %rsp
     * 16-byte aligned at process entry.  The pad -- when needed -- goes
     * HERE, above AT_NULL, where no libc auxv scan can ever reach it.
     *
     * It used to sit between envp's NULL and the first auxv pair as an
     * UNWRITTEN (hence zero) stack word: musl stops its auxv walk at the
     * first zero type, saw no auxv at all, computed its load base as 0
     * and died in _dlstart_c self-relocation -- killing exactly those
     * dynamically-linked binaries whose argument lengths happened to
     * require the pad, while others booted fine.
     */
    {
        uint64_t vec = 8ULL * (uint64_t)(argc + envc + 3);
        uint64_t top = sp & ~15ULL;
        if (((top - vec) & 15) != 0)
            top -= 8;
        sp = top;
    }
    if (!push_aux(as, &sp, AT_NULL, 0))            return 0;
    if (!push_aux(as, &sp, AT_ENTRY, entry))       return 0;
    if (!push_aux(as, &sp, AT_PAGESZ, 4096))       return 0;
    if (!push_aux(as, &sp, AT_CLKTCK, SCHED_HZ))   return 0;
    if (!push_aux(as, &sp, AT_SECURE, 0))          return 0;
    /* musl computes libc.secure as "(aux[0]&0x7800)!=0x7800 || uid!=euid ||
     * gid!=egid || AT_SECURE": the 0x7800 mask covers exactly these four
     * entries, so leaving any of them out marks EVERY process as running
     * setuid and musl's secure_getenv() then returns NULL unconditionally --
     * which is how libxkbcommon ended up blind to XKB_CONFIG_ROOT. */
    if (!push_aux(as, &sp, AT_UID, 0))             return 0;
    if (!push_aux(as, &sp, AT_EUID, 0))            return 0;
    if (!push_aux(as, &sp, AT_GID, 0))             return 0;
    if (!push_aux(as, &sp, AT_EGID, 0))            return 0;
    if (!push_aux(as, &sp, AT_RANDOM, at_random))  return 0;
    /* AT_BASE: the load address of the dynamic linker, if there is one.
     * musl's dynlink derives its own base from this (falling back to
     * AT_PHDR & -4096 when absent); a statically linked process has no
     * linker and gets no entry, which is exactly the Linux behaviour. */
    if (ldso_base) {
        if (!push_aux(as, &sp, AT_BASE, ldso_base))  return 0;
        dbg_puts("EXEC: pushed AT_BASE=");
        dbg_puts_hex(ldso_base);
        dbg_puts(" at sp=");
        dbg_puts_hex(sp);
        dbg_puts("\r\n");
    }
    if (phnum) {
        dbg_puts("EXEC: auxv phnum=");
        dbg_puts_dec((uint32_t)phnum);
        dbg_puts(" phdr=");
        dbg_puts_hex(phdr);
        dbg_puts("\r\n");
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
     * pushed.  The alignment itself is done above, before AT_NULL is pushed,
     * so no hole exists inside the vector. */

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
    char interp[64];
    /* ENOEXEC, not EINVAL: "I read the file and it is not something I can
     * run" is a distinct answer from "your arguments were wrong", and bash
     * keys its fallback-to-shell-script behaviour off exactly this errno. */
    if (load_executable(as, g_image, size, DYN_PROG_BASE, &entry, &phdr,
                        &phnum, interp, sizeof interp) <= 0) {
        vmm_destroy(as);
        return -E_NOEXEC;
    }

    /* A PT_INTERP in the image means this is a dynamically linked program:
     * the interpreter (ld-musl) is a second ELF, loaded at LDSO_BASE, and it
     * -- not the main program -- receives the entry point.  The main
     * program's own entry travels to it via AT_ENTRY; AT_BASE tells it where
     * it lives.  Load the interpreter into the same fresh address space. */
    uint64_t ldso_base = 0;
    /* Linux semantics: AT_ENTRY always names the MAIN program's e_entry,
     * even though the kernel starts the process at the interpreter's entry.
     * musl's __dls3 ends with CRTJMP(aux[AT_ENTRY], argv-1) -- handing off
     * to whatever AT_ENTRY names.  Pushing the interpreter's own entry made
     * every dynamically linked program re-run ld.so from _start after the
     * link completed: the second __dls3 pass re-ran with mallocng already
     * initialised and died in calloc's allzero fast path (rc=139), and any
     * binary that survived a couple of passes only did so by luck. */
    uint64_t app_entry = entry;
    if (interp[0]) {
        uint32_t isize = 0;
        if (!vfs_read_all(interp, g_image, sizeof(g_image), &isize)) {
            vmm_destroy(as);
            return -E_NOENT;
        }
        uint64_t ientry = 0;
        uint64_t iphdr = 0;
        uint16_t iphnum = 0;
        if (load_executable(as, g_image, isize, LDSO_BASE, &ientry, &iphdr,
                            &iphnum, NULL, 0) <= 0) {
            vmm_destroy(as);
            return -E_NOEXEC;
        }
        ldso_base = LDSO_BASE;
        entry = ientry;             /* the process starts in the linker */
        dbg_puts("EXEC: dyn interp=");
        dbg_puts(interp);
        dbg_puts(" ientry=");
        dbg_puts_hex(ientry);
        dbg_puts(" ldso_base=");
        dbg_puts_hex(ldso_base);
        dbg_puts(" main_entry=");
        dbg_puts_hex(app_entry);
        dbg_puts(" main_phdr=");
        dbg_puts_hex(phdr);
        dbg_puts("\r\n");
    }

    if (!vmm_alloc_range(as, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE,
                         VM_USER | VM_WRITE)) {
        vmm_destroy(as);
        return -E_NOMEM;
    }

    uint64_t sp = push_args(as, USER_STACK_TOP - 16, argv, argc, envp, envc,
                            phdr, phnum, app_entry, ldso_base);
    if (!sp) {
        vmm_destroy(as);
        return -E_NOMEM;
    }

    /* DEBUG: dump the user stack words from sp upward so the auxv layout can
     * be verified against what musl's _dlstart_c expects. */
    dbg_puts("EXEC: stack dump argc=");
    dbg_puts_dec((uint32_t)argc);
    dbg_puts(" envc=");
    dbg_puts_dec((uint32_t)envc);
    dbg_puts(" sp=");
    dbg_puts_hex(sp);
    dbg_puts("\r\n");
    {
        uint64_t addr = sp;
        for (int w = 0; w < 24; w++) {
            uint64_t pa = vmm_resolve(as, addr);
            uint32_t val = 0;
            if (pa) {
                const uint8_t *k = (const uint8_t *)pmm_virt(pa);
                val = *(const uint32_t *)(k + (addr & 0xFFF));
            }
            dbg_puts_hex((uint64_t)addr);
            dbg_puts(": ");
            dbg_puts_hex((uint64_t)val);
            dbg_puts("\r\n");
            addr += 8;
        }
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

    /* PID 1's controlling terminal is the first one, the same console the
     * kernel has been logging to.  It has to be set before the open below:
     * /dev/tty resolves against exactly this field. */
    p->ctty = 0;

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
    proc_make_runnable(p);

    dbg_puts("PROC: init is pid ");
    dbg_puts_dec((uint32_t)p->pid);
    dbg_puts(", entry ");
    dbg_puts_hex(entry);
    dbg_puts("\r\n");
    return p->pid;
}

/* ---- kernel threads ----------------------------------------------------
 * A kthread is a task whose entry is a kernel function, not a user program:
 * it runs on the shared kernel page tables (vmm_kernel_as), owns no user
 * memory, and its first scheduling returns into kthread_trampoline instead
 * of ret_to_user.  The fabricated switch_context frame plants the bootstrap
 * block in R12; the trampoline hands it to kthread_bootstrap() which runs
 * entry(arg) and then exits the thread. */

void kthread_bootstrap(kthread_bootstrap_t *b)
{
    void    (*entry)(void *) = b->entry;
    void    *arg             = b->arg;
    proc_t  *me              = proc_current();

    kfree(b);                   /* entry may need the heap */

    entry(arg);

    /* The thread is done.  It ran on the shared kernel page tables; drop
     * the address-space reference before proc_exit tears the task down,
     * or proc_teardown would destroy the kernel PML4 under our feet. */
    if (me)
        me->as = NULL;
    proc_exit(0);
    __builtin_unreachable();
}

proc_t *kthread_create(const char *name, void (*entry)(void *), void *arg)
{
    proc_t *p = proc_alloc();
    if (!p)
        return NULL;

    kthread_bootstrap_t *b = kmalloc(sizeof(*b));
    if (!b)
        return NULL;            /* slot left PROC_UNUSED: reusable */

    b->entry = entry;
    b->arg   = arg;

    p->as       = vmm_kernel_as();
    p->fs_base  = 0;
    p->uid = p->euid = p->suid = 0;     /* kernel threads are root */
    if (name)
        strncpy(p->name, name, sizeof(p->name) - 1);

    /* Fabricate a switch_context frame: six callee-saved registers (R12 =
     * bootstrap) and a return address of kthread_trampoline, exactly the
     * shape ret_to_user frames have except for who the return lands on. */
    uint64_t sp = p->kstack_top;
    sp -= 8;
    *(uint64_t *)(uintptr_t)sp = (uint64_t)(uintptr_t)kthread_trampoline;
    sp -= 6 * 8;
    uint64_t *regs = (uint64_t *)(uintptr_t)sp;
    regs[0] = 0;                            /* r15 */
    regs[1] = 0;                            /* r14 */
    regs[2] = 0;                            /* r13 */
    regs[3] = (uint64_t)(uintptr_t)b;       /* r12 */
    regs[4] = 0;                            /* rbx */
    regs[5] = 0;                            /* rbp */
    p->saved_rsp = sp;

    proc_make_runnable(p);
    return p;
}

/* ---- credentials ------------------------------------------------------- */
/*
 * Who you are is inherited, never derived: a child of a process running as
 * uid 1000 is uid 1000, and the only things that ever change that are the
 * set*id syscalls and a set-user-ID exec.  Same for the controlling terminal,
 * which is why a command typed at a shell on tty4 opens tty4 when it opens
 * /dev/tty.
 */
static void inherit_creds(proc_t *child, const proc_t *parent)
{
    child->uid  = parent->uid;
    child->euid = parent->euid;
    child->suid = parent->suid;
    child->gid  = parent->gid;
    child->egid = parent->egid;
    child->sgid = parent->sgid;
    child->ngroups = parent->ngroups;
    for (uint32_t i = 0; i < parent->ngroups; i++)
        child->groups[i] = parent->groups[i];
    child->ctty = parent->ctty;
}

int proc_in_group(const proc_t *p, uint32_t gid)
{
    if (!p)
        return 0;
    if (p->egid == gid)
        return 1;
    for (uint32_t i = 0; i < p->ngroups; i++)
        if (p->groups[i] == gid)
            return 1;
    return 0;
}

int proc_permitted(uint32_t mode, uint32_t uid, uint32_t gid, int want,
                   int is_dir)
{
    proc_t *p = proc_current();

    /* Before there is a scheduler there is no one to check against, and the
     * kernel's own boot-time opens have to succeed. */
    if (!p)
        return 1;
    if (!want)
        return 1;

    if (p->euid == 0) {
        /*
         * Root ignores the permission bits -- except that "executable" is
         * not a privilege, it is a property of the file.  Linux refuses even
         * root an execve of a file with no x bit anywhere, and so do we: it
         * is what stops a stray `./notes.txt` reaching the ELF loader.  A
         * directory's x bit means "searchable", which root always may.
         */
        if ((want & 1) && !is_dir && !(mode & 0111))
            return 0;
        return 1;
    }

    int shift = (p->euid == uid)          ? 6
              : proc_in_group(p, gid)     ? 3
              :                             0;
    return ((mode >> shift) & (uint32_t)want) == (uint32_t)want;
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
    /* The current directory and the file-creation mask are inherited too;
     * that is what lets a shell `cd` once and have every command it forks
     * start there. */
    strncpy(child->cwd, parent->cwd, GNUOS_PATH_MAX - 1);
    child->umask           = parent->umask;
    inherit_creds(child, parent);
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
    proc_make_runnable(child);

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
#define CLONE_THREAD       0x00010000
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

    /*
     * fork() gets a private copy of the address space (no COW, an eager
     * copy -- fork here is almost always followed by execve).  A thread
     * (CLONE_VM) shares it: both tasks keep running on the same page
     * tables, each holding one reference, and the last one out destroys
     * it.
     */
    if (flags & CLONE_VM)
        child->as = vmm_share(parent->as);
    else
        child->as = vmm_clone(parent->as);
    if (!child->as) {
        child->state = PROC_UNUSED;
        return -E_NOMEM;
    }

    child->ppid        = parent->pid;
    /*
     * A thread joins the parent's thread group, so both report the same
     * getpid(); the leader (pid == tgid) is what the grandparent waits on.
     * Plain fork gives the child a group of its own, which is why the
     * default from proc_alloc already fits and only this flag changes it.
     */
    if (flags & CLONE_THREAD)
        child->tgid = parent->tgid;
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
    strncpy(child->cwd, parent->cwd, GNUOS_PATH_MAX - 1);
    child->umask       = parent->umask;
    inherit_creds(child, parent);
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
    proc_make_runnable(child);

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

    /*
     * The execute bit is checked against the file the caller named, before
     * any `#!` chasing: a script the user may not execute stays unexecutable
     * however friendly its interpreter is.  The same lookup hands us the
     * setuid/setgid bits, which are only honoured for a real ELF -- setuid on
     * a script is a well-known way to hand out a root shell by accident, and
     * Linux has ignored it for thirty years.
     */
    uint32_t f_uid = 0, f_gid = 0, f_mode = 0;
    int      f_dir = 0;
    {
        int r2 = vfs_owner(pathbuf, &f_uid, &f_gid, &f_mode, &f_dir, 1);
        if (r2 < 0)
            return r2;
        if (f_dir)
            return -E_ACCES;
        if (!proc_permitted(f_mode, f_uid, f_gid, 1, 0))
            return -E_ACCES;
    }

    /* Chase `#!` lines until we reach something the ELF loader can take. */
    int interp_used = 0;
    for (int depth = 0; ; depth++) {
        if (depth >= EXEC_INTERP_MAX)
            return -E_LOOP;
        int got = resolve_interp(pathbuf, g_args, &argc);
        if (got < 0)
            return got;
        if (got == 0)
            break;
        interp_used = 1;
        g_args[argc] = NULL;
    }

    addrspace_t *as;
    uint64_t entry, sp;
    int rc = build_image(pathbuf, g_args, argc, g_envs, envc, &as, &entry, &sp);
    if (rc < 0)
        return rc;

    /*
     * execve() in a multithreaded process de-threads it: every other
     * member is sentenced to die (it is still holding the old address
     * space, which is about to be torn down), and the calling thread
     * becomes the group leader, exactly as Linux does.
     */
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *q = &g_procs[i];
        if (q->state == PROC_UNUSED || q == p || q->tgid != p->tgid)
            continue;
        q->group_dying = 1;
        if (q->state == PROC_BLOCKED)
            sched_wake(q);
    }
    if (p->pid != p->tgid)
        p->pid = p->tgid;

    /* Past this point the old image is gone and there is no way back. */
    addrspace_t *old = p->as;
    p->as = as;
    vmm_switch(as);
    /* The dying threads still hold references to `old`; the last of them
     * to exit tears it down. */
    vmm_put(old);

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
    /* execve starts a fresh program image, so drop the old address space's
     * mmap records; the page tables are rebuilt for the new binary. */
    if (p->as)
        p->as->nmmaps = 0;

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

    /*
     * Apply setuid/setgid only when we ran an ELF directly (no `#!` involved):
     * the owner of the binary becomes the effective, and saved, id.  A setuid
     * program that then drops privilege can still get it back via the saved
     * id, which is the whole point of having one.
     */
    if (!interp_used) {
        int suid = (f_mode & 04000) != 0;
        int sgid = (f_mode & 02000) != 0;
        if (suid) {
            p->euid = f_uid;
            p->suid = f_uid;
        }
        if (sgid) {
            p->egid = f_gid;
            p->sgid = f_gid;
        }
    }
    return 0;
}

/* ---- exit / wait ------------------------------------------------------ */
/*
 * The mechanical half of dying, shared by every exit path: close the
 * descriptors, clear the tid word pthread_join() waits on, and drop the
 * address-space reference.  `self` says whether `p` is the currently
 * running task -- when it is not (the group leader being closed out by a
 * dying sibling), the tid word belongs to somebody else's join and must be
 * left alone, and we cannot switch away from an address space that is not
 * the one we are standing on.
 */
static void proc_teardown(proc_t *p, int self)
{
    for (int i = 0; i < PROC_MAX_FD; i++) {
        if (p->fds[i] >= 0) {
            vfs_file_unref(p->fds[i]);
            p->fds[i] = -1;
        }
    }

    /* Futex-based pthread_join relies on the kernel zeroing the thread's
     * tid word when it dies; musl set the address via set_tid_address and
     * waits on it.  Do this before we drop the address space. */
    if (self && p->clear_child_tid && p->as) {
        uint64_t z = 0;
        vmm_copy_to_user(p->as, p->clear_child_tid, &z, 8);
        proc_wake_futex(p->as, p->clear_child_tid);
    }

    if (p->as) {
        /* We are still running on this address space, so step off it first
         * -- but only when the put below actually destroys it.  A thread
         * exiting from a live group keeps the space alive for the others
         * and must keep running on it until the scheduler moves us away. */
        if (self && p->as->refs <= 1)
            vmm_switch_kernel();
        vmm_put(p->as);
        p->as = NULL;
    }

    /* Orphans are adopted by init, which is the only process guaranteed to
     * still be around to reap them.  Members of our own thread group are
     * not orphans -- they are dying with us -- so leave them alone. */
    for (int i = 0; i < MAX_PROCS; i++) {
        if (g_procs[i].state != PROC_UNUSED && g_procs[i].ppid == p->pid &&
            g_procs[i].tgid != p->tgid)
            g_procs[i].ppid = 1;
    }
}

/* Wake the parent up and let it reap the group's zombie. */
static void proc_notify_parent(proc_t *p)
{
    proc_t *parent = proc_by_pid(p->ppid);
    if (parent) {
        proc_signal(parent, SIGCHLD);
        if (parent->state == PROC_BLOCKED && parent->wait_reason == WAIT_CHILD)
            sched_wake(parent);
    }
}

/*
 * One thread of a group is going away and the group lives on.  Its zombie
 * stays parked until the group leader is reaped, which is the one moment
 * everything the thread owned is known to be unreachable; proc_waitpid()
 * sweeps it up then.
 */
void proc_exit(int status)
{
    proc_t *p = g_current;

    /* The group leader exiting is a whole-group exit: the process the
     * parent waits for is going away, so its threads have to go too. */
    if (p->pid == p->tgid) {
        proc_exit_group(status);
        /* not reached */
    }

    proc_teardown(p, 1);

    p->exit_status = status;
    p->state       = PROC_ZOMBIE;

    sched_yield();
    panic("proc_exit: scheduled a zombie");
}

/*
 * Tear the whole thread group down.  Whoever called it -- leader or any
 * thread -- has already decided the process is over; every other member is
 * sentenced to die and woken if it was sleeping, so it exits on its way
 * back to user mode.  The leader becomes the one zombie the parent waits
 * on, carrying the exit status; the rest are swept at reap time.
 */
void proc_exit_group(int status)
{
    proc_t *p      = g_current;
    proc_t *leader = (p->pid == p->tgid) ? p : proc_by_pid(p->tgid);

    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *q = &g_procs[i];
        if (q->state == PROC_UNUSED || q->tgid != p->tgid)
            continue;
        q->group_dying = 1;
        if (q != p && q->state == PROC_BLOCKED)
            sched_wake(q);          /* READY ones die when next scheduled */
    }

    /*
     * If the caller is not the leader, the leader will never run again and
     * its share of the group's resources has to be released here, on its
     * behalf.  Do that before our own teardown: both hold a reference to
     * the same address space, and our own put may be the one that destroys
     * it -- which must not happen while we are still running on it.
     */
    if (leader != p) {
        /* The leader will never run again; if it is sitting in the run
         * queue, take it out before zombifying it. */
        spin_lock(&g_proc_lock);
        rq_remove(leader);
        spin_unlock(&g_proc_lock);
        proc_teardown(leader, 0);
        leader->exit_status = status;
        leader->term_sig    = p->term_sig;
        leader->state       = PROC_ZOMBIE;
    }

    proc_teardown(p, 1);

    if (leader == p) {
        p->exit_status = status;
        p->state       = PROC_ZOMBIE;
        proc_notify_parent(p);
    } else {
        /* A thread zombie like any other; swept when the leader is reaped. */
        p->exit_status = status;
        p->state       = PROC_ZOMBIE;
    }

    sched_yield();
    panic("proc_exit_group: scheduled a zombie");
}

/*
 * Wake every process blocked in futex(WAIT) on `addr` inside `as`.  Two
 * processes in different address spaces can wait on the same virtual
 * address (their pages differ), so the key is the pair.  The kernel is
 * single-threaded and non-preemptive, so there is no race between a WAKE
 * scan and a WAIT parking itself: a WAIT checks the word, blocks, and only
 * then can anyone else run.
 */
int proc_wake_futex(addrspace_t *as, uint64_t addr)
{
    int n = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *q = &g_procs[i];
        if (q->state == PROC_BLOCKED && q->wait_reason == WAIT_FUTEX &&
            q->as == as && q->futex_addr == addr) {
            sched_wake(q);
            n++;
        }
    }
    return n;
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
            /* A thread is never waited for on its own: it is reaped with
             * its group leader, and waitpid(-1) must not mistake one for a
             * child process. */
            if (c->pid != c->tgid)
                continue;
            have_child = 1;

            if (c->state == PROC_ZOMBIE) {
                int cpid = c->pid;
                if (status)
                    *status = c->term_sig ? (c->term_sig & 0x7F)
                                          : ((c->exit_status & 0xFF) << 8);
                c->state = PROC_UNUSED;
                /* Reaping the leader reaps the whole group: every thread
                 * zombie of it is unreachable now and its slot is freed
                 * here, the one place that knows it is safe. */
                for (int j = 0; j < MAX_PROCS; j++) {
                    if (g_procs[j].state == PROC_ZOMBIE &&
                        g_procs[j].tgid == cpid && &g_procs[j] != c)
                        g_procs[j].state = PROC_UNUSED;
                }
                return cpid;
            }

            /* A ptrace stop is reported to the tracer unconditionally:
             * waitpid needs no WUNTRACED to see it, exactly like Linux.
             * The tracee is PROC_BLOCKED in ptrace_stop(), so it is the
             * ptrace_stopped marker -- not a PROC_STOPPED state -- that
             * identifies the stop. */
            if (c->traced && c->ptrace_stopped && !c->reported) {
                c->reported = 1;
                if (status)
                    *status = 0x7F | ((c->stop_sig & 0xFF) << 8);
                return c->pid;
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
            /* A ptrace stop is not a job-control stop: SIGCONT must not
             * unpark it -- only PTRACE_CONT/SYSCALL/DETACH can. */
            if (p->traced && p->ptrace_stopped)
                return 0;
            p->reported = 0;
            /* A stopped process is not queued; put it back with a fresh
             * slice, exactly like a wake. */
            spin_lock(&g_proc_lock);
            enqueue_fresh(p);
            spin_unlock(&g_proc_lock);
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
    else if (p->state == PROC_STOPPED && sig == SIGKILL) {
        spin_lock(&g_proc_lock);
        enqueue_fresh(p);
        spin_unlock(&g_proc_lock);
    }

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

    /*
     * A dying thread (exit_group / exec killed it while it was parked in a
     * syscall or preempted) must not resume user code -- its stack may be
     * gone.  Turn it into its zombie here, on the way out.
     */
    if (p->group_dying) {
        p->exit_status = 0;
        proc_exit(0);
        /* not reached */
    }

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
         * A traced process stops for its tracer before the signal is
         * disposed of: the stop reports `sig` to the tracer's waitpid(),
         * and the signal the tracer hands back with PTRACE_CONT is what
         * actually gets delivered.  SIGKILL is the one exception -- Linux
         * never lets the tracer veto that either.
         */
        if (p->traced && sig != SIGKILL) {
            ptrace_signal_stop(p, r, sig);
            continue;
        }

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
            proc_exit_group(0);
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
        /* A fatal signal kills the whole group, not just the thread that
         * happened to be running when it landed -- that is the difference
         * between pthreads working and a worker thread's SIGSEGV leaving a
         * half-dead process behind. */
        proc_exit_group(0);
    }
}

/* ---- EEVDF run queue -------------------------------------------------- */
/* A binary min-heap of runnable processes keyed on virtual deadline.  All
 * queue operations run with g_proc_lock held (their callers say so). */

static int rq_before(proc_t *a, proc_t *b)
{
    if (a->deadline != b->deadline)
        return a->deadline < b->deadline;
    return a->pid < b->pid;                 /* deterministic tie-break */
}

static void rq_sift_up(unsigned i)
{
    while (i > 0) {
        unsigned parent = (i - 1) / 2;
        if (!rq_before(g_rq[i], g_rq[parent]))
            break;
        proc_t *t = g_rq[i];
        g_rq[i] = g_rq[parent];
        g_rq[parent] = t;
        g_rq[i]->rq_index = (int)i;
        g_rq[parent]->rq_index = (int)parent;
        i = parent;
    }
}

static void rq_sift_down(unsigned i)
{
    for (;;) {
        unsigned l = 2 * i + 1;
        unsigned r = 2 * i + 2;
        unsigned best = i;
        if (l < g_rq_size && rq_before(g_rq[l], g_rq[best]))
            best = l;
        if (r < g_rq_size && rq_before(g_rq[r], g_rq[best]))
            best = r;
        if (best == i)
            break;
        proc_t *t = g_rq[i];
        g_rq[i] = g_rq[best];
        g_rq[best] = t;
        g_rq[i]->rq_index = (int)i;
        g_rq[best]->rq_index = (int)best;
        i = best;
    }
}

static void rq_push(proc_t *p)
{
    p->rq_index = (int)g_rq_size;
    g_rq[g_rq_size++] = p;
    rq_sift_up(g_rq_size - 1);
}

static proc_t *rq_peek(void)
{
    return g_rq_size ? g_rq[0] : NULL;
}

static proc_t *rq_pop(void)
{
    if (!g_rq_size)
        return NULL;
    proc_t *top = g_rq[0];
    proc_t *last = g_rq[--g_rq_size];
    top->rq_index = -1;
    if (g_rq_size) {
        g_rq[0] = last;
        last->rq_index = 0;
        rq_sift_down(0);
    }
    return top;
}

/* Take a process out of the heap, wherever it sits.  Caller holds
 * g_proc_lock; a no-op for a process that is not queued. */
static void rq_remove(proc_t *p)
{
    if (p->rq_index < 0)
        return;
    unsigned i = (unsigned)p->rq_index;
    proc_t *last = g_rq[--g_rq_size];
    p->rq_index = -1;
    if (i == g_rq_size)                 /* p was the last element */
        return;
    g_rq[i] = last;
    last->rq_index = (int)i;
    if (i > 0 && rq_before(last, g_rq[(i - 1) / 2]))
        rq_sift_up(i);
    else
        rq_sift_down(i);
}

/* Put a process on the run queue with a fresh slice.  Caller holds
 * g_proc_lock.  EEVDF: a waking, continued or brand-new task's lag is
 * reset -- it enters as if newly runnable at the current virtual time. */
static void enqueue_fresh(proc_t *p)
{
    p->vruntime = g_vtime;
    p->deadline = g_vtime + p->slice;
    p->state    = PROC_READY;
    if (p->rq_index < 0)
        rq_push(p);
}

/* Make a brand-new process runnable.  Used by fork/clone/init so all three
 * share the "fresh task" rules. */
static void proc_make_runnable(proc_t *p)
{
    spin_lock(&g_proc_lock);
    enqueue_fresh(p);
    spin_unlock(&g_proc_lock);
}

/*
 * Hand the current CPU to the next runnable process.  Called voluntarily
 * (sched_yield, sched_block) and from the timer interrupt (sched_tick).
 * Returns 1 if the CPU context was switched away (and this frame resumed
 * later), 0 if nothing was runnable and the caller may carry on.
 *
 * g_proc_lock is held *across* switch_context and released on the resume
 * path, Xv6-style: the parking side never runs again until another core
 * picks its process, so the release is always performed by whoever comes
 * back.  That closes the race where a task marked READY but not yet saved
 * would be picked by another core -- the picker cannot take the lock until
 * the switch that saved it has finished.
 */
static int schedule(void)
{
    spin_lock(&g_proc_lock);

    proc_t *prev = cpu_self()->current;

    /* Account the outgoing task's CPU time.  Virtual time only advances
     * while a process actually runs; the idle context (prev == NULL)
     * contributes nothing. */
    if (prev) {
        uint64_t now  = timer_ticks();
        uint64_t used = now - prev->last_run_tick;
        prev->vruntime += used;
        g_vtime        += used;
        prev->last_run_tick = now;
        if (prev->state == PROC_RUNNING) {
            prev->state = PROC_READY;
            /* A slice that has been fully consumed earns a fresh deadline
             * when it goes back on the queue; one that still has time left
             * keeps it, so the remainder is owed to the task (EEVDF). */
            if (prev->vruntime >= prev->deadline)
                prev->deadline = prev->vruntime + prev->slice;
            rq_push(prev);
        }
    }

    proc_t *next = rq_pop();
    if (!next) {
        /* Nothing runnable.  A caller that is still RUNNING may simply
         * carry on; a blocked one parks this CPU's context in the idle
         * slot until something wakes it. */
        if (!prev || prev->state == PROC_RUNNING) {
            spin_unlock(&g_proc_lock);
            return 0;
        }
        if (prev->bkl_held) {
            prev->bkl_held = 0;
            bkl_release();
        }
        cpu_self()->current = NULL;
        vmm_switch_kernel();
        switch_context(&prev->saved_rsp, cpu_self()->sched_rsp);
        spin_unlock(&g_proc_lock);       /* resumed: we are the idle context */
        return 1;
    }

    next->state = PROC_RUNNING;
    if (next == prev) {
        /* The queue held nothing else: we pushed and re-picked ourselves.
         * Nothing to load, nothing to park.  The BKL is still held from the
         * interrupt entry (isr_dispatch) or was dropped by bkl_leave_for_switch;
         * either way the caller's bkl_return_from_switch() will re-take it, so
         * hand it back here instead of deadlocking on our own acquisition. */
        if (prev && prev->bkl_held) {
            prev->bkl_held = 0;
            bkl_release();
        }
        spin_unlock(&g_proc_lock);
        return 1;
    }

    next->deadline      = next->vruntime + next->slice;  /* fresh slice */
    next->last_run_tick = timer_ticks();
    int fresh = (next->on_cpu == -1);   /* never ran: fabricated stack */
    next->on_cpu        = cpu_self()->id;
    cpu_self()->current = next;

    /* Park the outgoing FPU state, then switch the stack, the TLS base,
     * the page tables and the FPU of the incoming task. */
    if (prev)
        fpu_save(prev->fpu);
    tss_set_rsp0(next->kstack_top);
    proc_set_fs(next);
    vmm_switch(next->as);
    fpu_load(next->fpu);

    /* A parked process never owns the big kernel lock: whoever resumes it
     * re-takes it (sched_tick/sched_block/sched_yield all do).  Dropping
     * it here is what stops one core's timer interrupt from spinning
     * forever on a lock a descheduled process is holding. */
    if (prev && prev->bkl_held) {
        prev->bkl_held = 0;
        bkl_release();
    }

    /* A brand-new task's stack (built by build_startup_stack) returns
     * straight into ret_to_user: it has no schedule() frame that would
     * run the `spin_unlock` below on resume, so if we carry g_proc_lock
     * across the switch the first timer tick deadlocks on it.  Drop the
     * lock before handing the CPU over. */
    if (fresh)
        spin_unlock(&g_proc_lock);

    uint64_t *save = prev ? &prev->saved_rsp : &cpu_self()->sched_rsp;
    switch_context(save, next->saved_rsp);
    spin_unlock(&g_proc_lock);           /* resumed: inherit this core's lock */
    return 1;
}

/* Drop the BKL before parking this process's kernel execution and re-take
 * it on resume, so the lock never rides a descheduled context. */
static void bkl_leave_for_switch(proc_t *p)
{
    if (p && p->bkl_held) {
        p->bkl_held = 0;
        bkl_release();
    }
}

static void bkl_return_from_switch(void)
{
    proc_t *p = cpu_self()->current;
    if (p) {
        bkl_acquire();
        p->bkl_held = 1;
    }
}

void sched_yield(void)
{
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags));

    bkl_leave_for_switch(cpu_self()->current);
    schedule();
    bkl_return_from_switch();

    if (flags & 0x200)
        asm volatile("sti");
}

void sched_tick(void)
{
    /* Already inside an interrupt gate, so interrupts are off.  The BKL was
     * taken on entry from ring 3 (see isr_dispatch); a preempted task's
     * frame parks without it and re-takes it on resume below. */
    proc_t *cur = cpu_self()->current;
    if (!cur)
        return;

    spin_lock(&g_proc_lock);
    uint64_t now  = timer_ticks();
    cur->vruntime += now - cur->last_run_tick;
    g_vtime       += now - cur->last_run_tick;
    cur->last_run_tick = now;
    proc_t *head  = rq_peek();
    int preempt   = (cur->vruntime >= cur->deadline) ||    /* slice gone */
                    (head && head->deadline < cur->deadline); /* owed task */
    spin_unlock(&g_proc_lock);

    if (preempt) {
        schedule();
        bkl_return_from_switch();
    }
}

void sched_block(wait_reason_t why)
{
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags));

    proc_t *cur = cpu_self()->current;
    if (cur) {
        cur->state       = PROC_BLOCKED;
        cur->wait_reason = why;
        bkl_leave_for_switch(cur);
        schedule();
        bkl_return_from_switch();
    }

    if (flags & 0x200)
        asm volatile("sti");
}

void sched_block_irqoff(wait_reason_t why)
{
    proc_t *cur = cpu_self()->current;
    if (!cur)
        return;
    cur->state       = PROC_BLOCKED;
    cur->wait_reason = why;
    bkl_leave_for_switch(cur);
    schedule();
    bkl_return_from_switch();
}

void sched_block_timeout(wait_reason_t why, uint64_t ticks)
{
    proc_t *p = cpu_self()->current;
    if (!p)
        return;
    p->state       = PROC_BLOCKED;
    p->wait_reason = why;
    p->wake_tick   = timer_ticks() + (ticks ? ticks : 1);
    bkl_leave_for_switch(p);
    schedule();
    bkl_return_from_switch();
    p->wake_tick   = 0;
}

void sched_wake(proc_t *p)
{
    if (!p)
        return;
    spin_lock(&g_proc_lock);
    if (p->state == PROC_BLOCKED) {
        p->wait_reason = WAIT_NONE;
        p->wake_tick   = 0;
        enqueue_fresh(p);
    }
    spin_unlock(&g_proc_lock);
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

void sched_wake_queue(void *q)
{
    for (int i = 0; i < MAX_PROCS; i++)
        if (g_procs[i].state == PROC_BLOCKED && g_procs[i].wait_q == q)
            sched_wake(&g_procs[i]);
}

/*
 * The boot thread of every core becomes the same idle loop.  It never
 * dies: whenever every process is blocked, schedule() comes back here and
 * we halt until the next interrupt makes somebody runnable again.
 */
void sched_start(void)
{
    dbg_puts("SCHED: entering the run loop\r\n");

    for (;;) {
        asm volatile("cli");
        if (!schedule()) {
            /* sti and hlt must be adjacent: sti only unmasks after the next
             * instruction, which closes the wake-up race. */
            asm volatile("sti; hlt");
        }
    }
}
