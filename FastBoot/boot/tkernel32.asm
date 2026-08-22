; ================================================================
; tkernel32.asm — FastBoot 32 位测试内核（完整版 v2）
; 由 stage2 保护模式跳入（0x100000），ESI = BootInfo（0x10000）
; 从 BootInfo 读真实分辨率/帧缓冲，画 VBE：蓝底 + 中央红块 + 底部绿条
; ================================================================
[ORG 0x100000]
BITS 32

start32:
    mov ebp, esi                ; BootInfo
    mov edi, [ebp + 24]         ; framebuffer
    test edi, edi
    jnz .have_fb
    ; 无帧缓冲：写 VGA 文本
    mov edi, 0xB8000
    mov ecx, 80 * 25
    mov eax, 0x0A20
    rep stosw
    mov esi, msg_text
.lt:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0A
    mov [0xB8000 + 160], ax
    inc esi
    jmp .lt
.have_fb:
    mov esi, [ebp + 12]         ; width
    mov edx, [ebp + 16]         ; height
    ; 全屏蓝（字节序 R|B<<8|G<<16）
    mov ecx, esi
    imul ecx, edx
    mov eax, 0x0000FF00          ; 蓝
    rep stosd
    ; 中央红块 200x120：(w/2-100, h/2-60) 起
    mov edi, [ebp + 24]
    mov eax, esi
    mov ecx, edx
    shr ecx, 1
    sub ecx, 60
    imul eax, ecx
    shl eax, 2
    add edi, eax
    mov eax, esi
    shr eax, 1
    sub eax, 100
    shl eax, 2
    add edi, eax
    mov ecx, 120
.row:
    push ecx
    push esi
    mov ecx, 200
    mov eax, 0x000000FF          ; 红
    rep stosd
    pop esi
    pop ecx
    mov eax, esi
    sub eax, 200
    shl eax, 2
    add edi, eax
    loop .row
    ; 底部绿条（高 40）
    mov edi, [ebp + 24]
    mov eax, esi
    mov ecx, edx
    sub ecx, 40
    imul eax, ecx
    shl eax, 2
    add edi, eax
    mov ecx, 40
.rowg:
    push ecx
    mov ecx, esi
    mov eax, 0x00FF0000          ; 绿
    rep stosd
    pop ecx
    loop .rowg
.done:
    hlt
    jmp .done

msg_text db 'FastBoot: 32-bit kernel (no framebuffer)', 0

times 4096-($-$$) db 0          ; 内核占满 8 扇区（扇区 10..17）
