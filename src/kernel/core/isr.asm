; isr.asm — interrupt/exception entry stubs. (GPLv2)
;
; One stub per vector, all of them padded to a fixed 16-byte stride so that
; idt_init() can compute the address of vector N as isr_stub_base + N*16
; without needing a 256-entry pointer table (which, in a PIE kernel, would
; have to be relocated at load time).
;
; Each stub normalises the stack into the layout of `regs_t` (see panic.h):
;
;   low   r15 r14 r13 r12 r11 r10 r9 r8 rbp rdi rsi rdx rcx rbx rax
;         vector errcode
;   high  rip cs rflags rsp ss           <- pushed by the CPU
;
; The CPU only pushes an error code for some exceptions, so the stubs for the
; other vectors push a dummy zero to keep the frame uniform.

bits 64

extern isr_dispatch
global isr_stub_base

section .text

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    cld                     ; SysV ABI: DF must be clear on entry to C
    mov  rdi, rsp           ; rdi = regs_t *
    call isr_dispatch

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16             ; discard vector + error code
    iretq

align 16
isr_stub_base:
%assign vec 0
%rep 256
    align 16
  %if (vec == 8) || (vec == 10) || (vec == 11) || (vec == 12) || (vec == 13) || (vec == 14) || (vec == 17) || (vec == 21) || (vec == 29) || (vec == 30)
    ; the CPU already pushed a real error code
  %else
    push qword 0
  %endif
    push qword vec
    jmp  isr_common
%assign vec vec+1
%endrep

; Tell the linker we do not need an executable stack.
section .note.GNU-stack noalloc noexec nowrite progbits

; ---------------------------------------------------------------------------
; syscall instruction entry (AMD/Intel "syscall", as used by musl/glibc).
;
; Unlike int 0x80, the CPU does NOT push a frame and does NOT switch stacks:
; on entry RCX holds the user RIP, R11 holds the user RFLAGS, and RSP is still
; the user stack.  We stash RSP in a per-CPU scratch word (interrupts are off
; and the scratch is reached through GS, so no other core can clobber it),
; switch to the running process's kernel stack -- the per-CPU mirror of
; TSS.RSP0, so this path lands exactly where a trap through isr_stub_base
; would have, and a syscall that blocks parks its frame somewhere no other
; process can tread -- build the same regs_t frame isr_stub_base would have
; made -- note the pushes must run high address to low address, i.e. ss first
; and r15 last, or the frame comes out mirrored -- then call the shared
; syscall_handler(), then iretq back.  (iretq lets us use the real user code/
; data selectors instead of dancing with IA32_STAR's sysret CS arithmetic.)
; RCX and R11 are clobbered by the instruction and cannot be recovered -- that
; is the normal x86-64 syscall ABI, and musl saves them itself.
;
; The two per-CPU slots are struct cpu offsets (smp.h); GS base points at the
; current core's cpu_t, so these reads are per-CPU by construction.
; ---------------------------------------------------------------------------
%define CPU_KERNEL_RSP0 32
%define CPU_USER_RSP    40

section .text
global syscall_entry
syscall_entry:
    cli                         ; run the handler with interrupts off
    mov [gs:CPU_USER_RSP], rsp
    mov rsp, [gs:CPU_KERNEL_RSP0]    ; this process's kernel stack

    ; Build the frame in *descending* address order, exactly like the CPU +
    ; isr_stub_base pair does, so that the final RSP points at regs_t.r15.
    push qword 0x1B                  ; ss     = SEL_UDATA
    push qword [gs:CPU_USER_RSP]     ; rsp    = user RSP
    push r11                         ; rflags = user RFLAGS (syscall put it here)
    push qword 0x23                  ; cs     = SEL_UCODE
    push rcx                         ; rip    = user RIP  (syscall put it here)
    push qword 0                     ; errcode
    push qword 0x80            ; vector = SYSCALL_VECTOR (so isr_dispatch
                                ;          routes us to g_syscall)

    ; regs_t GP registers, in the same order isr_common pushes them.
    push rax
    push rbx
    push rcx                   ; user RCX is lost; harmless (see note above)
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    cld                        ; SysV ABI: DF must be clear on entry to C
    mov rdi, rsp

    ; Hand the frame to isr_dispatch -- the exact same routine the int 0x80
    ; stub calls.  It sees vector 0x80, invokes g_syscall(), and on return we
    ; restore the frame and iretq.  Calling isr_dispatch (not syscall_handler
    ; directly) keeps the only cross-object reference resolvable at link time
    ; without a PLT, which this PIE kernel forbids in read-only .text.
    call isr_dispatch

    ; restore GP registers, then let iretq pop rip/cs/rflags/rsp/ss.
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                ; discard vector + errcode
    iretq
