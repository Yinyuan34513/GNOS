; ================================================================
; tkernel.asm — FastBoot 测试内核（BIOS 侧，加载到 0x1000:0 = 0x10000）
; 写 VGA 文本 + hlt
; ================================================================
[ORG 0x10000]
BITS 16

tk:
    mov ax, 0xB800
    mov es, ax
    xor di, di
    mov si, tk_msg
    mov ah, 0x0A       ; 亮绿黑底
.loop:
    lodsb
    or al, al
    jz .done
    mov [es:di], ax
    add di, 2
    jmp .loop
.done:
    hlt
    jmp .done

tk_msg db 'FastBoot: kernel booted (BIOS variant)', 0

times 512-($-$$) db 0
