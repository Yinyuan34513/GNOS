; crt0.asm — the user-space entry stub. (GPLv2)
;
; The kernel starts a process with RSP pointing at the SysV startup block it
; built in proc.c:
;
;     [rsp]      argc
;     [rsp+8]    argv[0]
;     ...
;     argv[argc] == NULL
;     envp[0]
;     ... envp[envc-1], envp[envc] == NULL, then auxv pairs
;     envp[0]    == NULL
;
; so all _start has to do is turn that into the (argc, argv) pair main()
; expects and make sure a main() that simply returns still calls exit().

bits 64

global _start
extern main
extern exit

section .text.start

_start:
    mov  rdi, [rsp]          ; argc
    lea  rsi, [rsp + 8]      ; argv
    lea  rax, [rsp + 8]
    lea  rdx, [rax + rdi*8 + 8] ; past argv[argc]==NULL: envp[0]
    xor  rbp, rbp            ; end of the frame-pointer chain
    call main
    mov  rdi, rax            ; main's return value becomes the exit status
    call exit
.hang:
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
