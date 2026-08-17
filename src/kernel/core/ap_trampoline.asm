; ap_trampoline.asm — AP start for GNOS SMP bring-up. (GPLv2)
;
; Limine starts each AP in 64-bit long mode and calls the goto_address we
; installed, with RDI pointing at that core's struct limine_smp(info).  At
; that point the AP has no usable stack of its own, so the very first thing
; we do is load RSP from a per-CPU array (g_ap_stack_top), then hand the cpu
; index (info->extra_argument) to ap_main() in C, which finishes the set-up.
;
; struct limine_smp(info) layout (x86-64):
;   0  uint32_t processor_id
;   4  uint32_t lapic_id
;   8  uint64_t reserved
;  16  void*    goto_address
;  24  uint64_t extra_argument   <- our cpu index

bits 64

global ap_entry
extern g_ap_stack_top
extern ap_main

section .text

ap_entry:
    mov  rax, [rdi + 24]        ; rax = cpu index (extra_argument)
    lea  rbx, [rel g_ap_stack_top]
    mov  rsp, [rbx + rax*8]     ; load this core's kernel stack
    mov  rdi, rax              ; argument: cpu index
    call ap_main

    ; ap_main does not return; if it ever does, park the core.
    cli
.halt:
    hlt
    jmp  .halt
