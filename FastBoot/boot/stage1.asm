; ================================================================
; stage1.asm — FastBoot BIOS 引导扇区（512B）
; 加载 stage2（扇区 2..9，8 个扇区）到 0x9000:0 并跳转
; ================================================================
[ORG 0x7C00]
BITS 16

start:
    mov ax, 0x07C0
    mov ds, ax
    mov ax, 0x9000
    mov es, ax
    xor bx, bx
    mov ah, 0x02
    mov al, 16          ; 读 16 扇区：stage2(8,→0x90000) + 内核32(8,→0x92000)
    mov ch, 0
    mov cl, 2           ; 从扇区 2 起（构建修复：内核由 stage1 一并加载，
                        ; 避免 stage2 的 int 0x13 读高扇区/1MB 边界失败）
    mov dh, 0
    mov dl, 0x00        ; 软盘（QEMU -fda）
    int 0x13
    jc disk_error
    jmp 0x9000:0x0000

disk_error:
    mov si, msg
.print:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp .print
.done:
    hlt
    jmp .done

msg db 'FastBoot: disk read error', 0

times 510-($-$$) db 0
dw 0xAA55
