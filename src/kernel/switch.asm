; switch.asm — kernel context switch and the new-task trampoline. (GPLv2)
;
; switch_context() is the only place where one task's kernel stack is swapped
; for another's.  It saves the callee-saved registers the SysV ABI says a
; function must preserve, parks RSP in the outgoing task's PCB, loads the
; incoming task's RSP and returns -- straight into whatever that task was
; doing when *it* called switch_context.
;
; A brand-new task has never called switch_context, so proc.c fabricates a
; stack that looks as if it had: six saved registers, a return address of
; ret_to_user, and above that a full regs_t frame.  Returning therefore lands
; in ret_to_user, which pops the frame and drops to ring 3.

bits 64

global switch_context
global ret_to_user

section .text

; void switch_context(uint64_t *save_rsp, uint64_t load_rsp)
switch_context:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov  [rdi], rsp         ; remember where we stopped
    mov  rsp, rsi           ; adopt the incoming task's stack

    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp
    ret

; Entered by "returning" into it from switch_context, with RSP pointing at a
; regs_t frame.  Identical to the tail of isr_common in isr.asm.
ret_to_user:
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

section .note.GNU-stack noalloc noexec nowrite progbits
