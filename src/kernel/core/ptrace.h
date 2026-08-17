#ifndef GNUCOS_PTRACE_H
#define GNUCOS_PTRACE_H

#include <stdint.h>
#include "panic.h"

/* ptrace(2) request numbers, Linux UAPI values.  These are a stable ABI --
 * glibc and musl both hand them through raw -- so they are hardcoded rather
 * than derived from anything. */
#define PTRACE_TRACEME    0
#define PTRACE_PEEKTEXT   1
#define PTRACE_PEEKDATA   2
#define PTRACE_PEEKUSR    3
#define PTRACE_POKETEXT   4
#define PTRACE_POKEDATA   5
#define PTRACE_POKEUSR    6
#define PTRACE_CONT       7
#define PTRACE_KILL       8
#define PTRACE_SINGLESTEP 9
#define PTRACE_GETREGS    12
#define PTRACE_SETREGS    13
#define PTRACE_ATTACH     16
#define PTRACE_DETACH     17
#define PTRACE_SYSCALL    24
#define PTRACE_SETOPTIONS 0x4200
#define PTRACE_GETEVENTMSG 0x4201
#define PTRACE_GETSIGINFO  0x4202
#define PTRACE_SETSIGINFO  0x4203
#define PTRACE_GETREGSET   0x4204
#define PTRACE_SETREGSET   0x4205

/* The single PTRACE_O_* option this kernel honours.  When it is set, a
 * syscall stop reports SIGTRAP|0x80 instead of SIGTRAP, so waitpid() can
 * tell a syscall-entry stop from a stray SIGTRAP the way strace(1) does. */
#define PTRACE_O_TRACESYSGOOD 0x00000001

/* Which of the two stop sites the tracee stops at next. */
#define PTRACE_RUN_CONT    0
#define PTRACE_RUN_SYSCALL 1

/* Linux struct user_regs_struct for x86-64, byte for byte: the layout
 * gdb/strace expect PTRACE_GETREGS to hand back.  The trap frame (regs_t)
 * holds a superset -- the interrupt vector and error code -- which does not
 * belong to the program, so the conversion drops it; orig_rax is the
 * syscall number, which Linux reports separately from rax because rax holds
 * the return value by the time anyone looks. */
typedef struct {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rax, rcx, rdx, rsi, rdi, orig_rax;
    uint64_t rip, cs, eflags, rsp, ss;
    uint64_t fs_base, gs_base, ds, es, fs, gs;
} ptrace_user_regs_t;

/* The kernel-side face of ptrace(2). */
int  sys_ptrace(int request, int64_t pid, uint64_t addr, uint64_t data);

/* Hooked into syscall_handler(): a tracee in PTRACE_RUN_SYSCALL mode stops
 * once before the call dispatches (the tracer sees the original register
 * image and may rewrite it) and once after it returns (rax holds the
 * result, exactly like Linux). */
void ptrace_syscall_enter(regs_t *r, uint64_t nr);
void ptrace_syscall_exit(regs_t *r);

/* Called from proc_check_signals() when a traced process has a deliverable
 * signal: the signal becomes a ptrace stop instead of being disposed of. */
void ptrace_signal_stop(proc_t *p, regs_t *r, int sig);

#endif