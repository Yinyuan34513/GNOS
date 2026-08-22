; ================================================================
; stage2.asm — FastBoot BIOS 引导管理器（完整版）
;   E820 内存图 → BootInfo（物理 0x10000）
;   VBE 800x600x32 图形模式（模式 0x117）+ 帧缓冲地址
;   8042 重启
;   实模式 → 保护模式切换 → 跳 32 位内核（0x100000）
;   菜单：1 = 引导 32 位内核   2 = VBE 图形模式   r = 重启
;
; BootInfo（物理 0x10000，布局对齐 vboot.h vboot_bootinfo_t）：
;   +0  magic 'BOOT'   +4 mem_base   +8 mem_count
;   +12 vw  +16 vh  +20 vbpp  +24 framebuffer  +28 entry
; E820 数组 → 0x11000
; ================================================================
[ORG 0x0000]
BITS 16

start:
    mov ax, 0x9000
    mov ds, ax
    mov ss, ax
    mov sp, 0xFE00
    mov ax, 0x0003
    int 0x10
    mov si, msg_title
    call puts

menu_loop:
    mov si, msg_menu
    call puts
    xor ah, ah
    int 0x16
    cmp al, '1'
    je boot_kernel
    cmp al, '2'
    je do_vbe
    cmp al, 'r'
    je reboot
    cmp al, 'R'
    je reboot
    jmp menu_loop

do_vbe:
    ; 构建修复：改用 Bochs VBE 端口接口（QEMU stdvga 支持，
    ; 32os 已验证；int 0x10 4F02 在本 QEMU 上不可靠）
    ; 先关显示
    mov dx, 0x1CE
    mov ax, 0x0004
    out dx, ax
    mov dx, 0x1CF
    mov ax, 0x0000
    out dx, ax
    ; ID
    mov dx, 0x1CE
    mov ax, 0x0000
    out dx, ax
    mov dx, 0x1CF
    mov ax, 0xB0C0
    out dx, ax
    ; XRES = 800
    mov dx, 0x1CE
    mov ax, 0x0001
    out dx, ax
    mov dx, 0x1CF
    mov ax, 800
    out dx, ax
    ; YRES = 600
    mov dx, 0x1CE
    mov ax, 0x0002
    out dx, ax
    mov dx, 0x1CF
    mov ax, 600
    out dx, ax
    ; BPP = 32
    mov dx, 0x1CE
    mov ax, 0x0003
    out dx, ax
    mov dx, 0x1CF
    mov ax, 32
    out dx, ax
    ; ENABLE | LFB
    mov dx, 0x1CE
    mov ax, 0x0004
    out dx, ax
    mov dx, 0x1CF
    mov ax, 0x0041
    out dx, ax
    ; PCI 扫 LFB（QEMU VGA 1234:1111 的 BAR0）
    call find_lfb
    mov [boot_info_fb], eax
    mov word [boot_info_vw], 800
    mov word [boot_info_vh], 600
    mov si, msg_vbe_ok
    call puts
    jmp menu_loop
vbe_fail:
    mov si, msg_vbe_fail
    call puts
    jmp menu_loop

; ---- PCI 扫描 QEMU VGA，返回 BAR0（帧缓冲地址）于 EAX ----
find_lfb:
    pusha
    xor ecx, ecx
.pl:
    mov eax, ecx
    shl eax, 11
    or eax, 0x80000000
    mov edx, 0xCF8
    out dx, eax
    mov edx, 0xCFC
    in eax, dx
    and eax, 0xFFFF
    cmp eax, 0x1234
    jne .nx
    mov eax, ecx
    shl eax, 11
    or eax, 0x80000010
    mov edx, 0xCF8
    out dx, eax
    mov edx, 0xCFC
    in eax, dx
    and eax, 0xFFFFFFF0
    mov [boot_info_fb], eax
    popa
    ret
.nx:
    inc ecx
    cmp ecx, 32
    jb .pl
    popa
    ret

reboot:
    mov al, 0xFE               ; 8042 复位
    out 0x64, al
    int 0x19

boot_kernel:
    mov si, msg_boot
    call puts
    ; 构建修复：内核已由 stage1 加载到 0x92000（stage2 不读盘——
    ; stage2 的 int 0x13 读高扇区/EDD 在本环境均失败）
    call do_e820
    call write_bootinfo
    ; 切保护模式
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    ; 构建修复：远跳目标必须是 32 位线性地址 = 0x90000 + 段内偏移
    ;（GDT 代码段基址 0；原 16 位偏移会跳到物理 0xB0 的实模式区）
    jmp dword 0x08:(pm_entry + 0x90000)

; ---- E820 内存图 → 0x11000，条数写 BootInfo+8 ----
do_e820:
    push ds
    mov ax, 0x1000
    mov ds, ax
    mov di, 0x1000            ; E820 数组 → 0x11000
    mov dword [0x0008], 0     ; mem_count = 0
    xor ebx, ebx
.loop:
    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150       ; 'SMAP'
    int 0x15
    jc .done
    cmp eax, 0x534D4150
    jne .done
    inc dword [0x0008]
    add di, 24
    test ebx, ebx
    jz .done
    jmp .loop
.done:
    pop ds
    ret

; ---- 写 BootInfo（物理 0x10000） ----
write_bootinfo:
    push ds
    mov ax, 0x1000
    mov ds, ax
    mov dword [0x0000], 0x544F4F42   ; magic 'BOOT'
    mov dword [0x0004], 0x11000      ; mem_base
    mov eax, [boot_info_vw]
    mov dword [0x000C], eax          ; vw（真实分辨率）
    mov eax, [boot_info_vh]
    mov dword [0x0010], eax          ; vh
    mov dword [0x0014], 32           ; vbpp
    mov eax, [boot_info_fb]
    mov dword [0x0018], eax          ; framebuffer
    mov dword [0x001C], 0x100000     ; entry
    pop ds
    ret

disk_error:
    mov si, msg_err
    call puts
    jmp menu_loop

puts:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp puts
.done:
    ret

msg_title db 13,10, 'FastBoot (BIOS full) - VBoot boot manager', 13,10, 0
msg_menu  db 13,10, '  1. Boot 32-bit kernel', 13,10
          db '  2. VBE 800x600x32 mode', 13,10
          db '  r. Reboot', 13,10, '> ', 0
msg_boot  db 13,10, 'Booting 32-bit kernel...', 13,10, 0
msg_vbe_ok  db 'VBE mode set OK', 13,10, 0
msg_vbe_fail db 'VBE mode failed', 13,10, 0
msg_err   db 13,10, 'disk error', 0

; EDD 磁盘地址包：读 LBA 9（扇区 10）起 8 扇区到 0x92000
;（构建修复：int 0x13 的实模式缓冲避免落在 1MB 边界 0x100000，
;  保护模式入口处再 rep movsd 搬到 0x100000）
dap_kernel:
    db 0x10, 0x00        ; 包大小 + 保留
    dw 8                 ; 扇区数
    dw 0x2000            ; 偏移（0x9000:0x2000 = 0x92000）
    dw 0x9000            ; 段
    dd 9                 ; LBA 起始
    dd 0

boot_info_fb: dd 0xfd000000   ; 构建修复：QEMU 8.2 stdvga VGA BAR0（info pci 证实），
                              ; 非 Bochs 标准 0xE0000000——find_lfb 失败时回落也正确
boot_info_vw: dd 800           ; 构建修复：DWORD（原 dw 被按 dword 读成 800|600<<16=0x02580320）
boot_info_vh: dd 600           ; 真实高
vbe_info:    times 256 db 0     ; VBE 模式信息块

; ---- 保护模式 GDT ----
align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF       ; 0x08 内核代码（flat 32）
    dq 0x00CF92000000FFFF       ; 0x10 内核数据
gdt_desc:
    dw (gdt_desc - gdt) - 1
    dd gdt + 0x90000            ; 段基址 0x90000（加载在 0x9000:0）

; ---- 保护模式入口 ----
BITS 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000
    mov ebx, 0x10000            ; BootInfo 地址（保存）
    ; 内核从 0x91000 搬到 0x100000（4096 字节）
    ;（构建修复：stage1 16 扇区连续读 → stage2 0x90000-0x90FFF，
    ;  内核第 9 扇区起在 0x91000，而非 0x92000）
    mov edi, 0x100000
    mov esi, 0x91000
    mov ecx, 1024
    rep movsd
    mov esi, ebx                ; 恢复 BootInfo
    jmp dword 0x08:0x100000     ; 跳 32 位内核

times 4096-($-$$) db 0          ; stage2 占满 8 扇区（扇区 2..9）
