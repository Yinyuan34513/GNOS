# ================================================================
# boot_x86.S — APinux OS x86_64 启动入口 (Multiboot2)
# 功能：切换到长模式，设置页表，跳转到 kernel_main
# ================================================================
# ================================================================
# 构建修复：QEMU 8.2 的 -kernel 只认 Multiboot v1 (0x1BADB002) 与
# bzImage/PVH，不认 Multiboot2 (0xE85250D6)。改为 v1 头。
# 关键：flags 必须置 bit16 (MULTIBOOT_HEADER_HAS_ADDR)——
# QEMU 的 load_multiboot 中，bit16 未置位时会把镜像当 ELF 加载
# （对 raw 文件报 "Error while loading elf kernel"），置位后才走
# 下方 load_addr/load_end/bss_end/entry 字段的 raw 加载路径，
# 加载到 1M 并把 [load_end, bss_end) 清零。
# ================================================================
.set MULTIBOOT_HEADER_MAGIC,   0x1BADB002
.set MULTIBOOT_HEADER_FLAGS,   0x00010000
.set MULTIBOOT_CHECKSUM,       -(0x1BADB002 + 0x00010000)

.section .multiboot_header
.align 4
header_start:
    .long MULTIBOOT_HEADER_MAGIC
    .long MULTIBOOT_HEADER_FLAGS
    .long MULTIBOOT_CHECKSUM
    .long header_start        # header_addr（链接在 1M，即 0x100000）
    .long header_start        # load_addr：加载到 1M
    .long _load_end           # load_end_addr
    .long _end                # bss_end_addr（引导器清零 [load_end, bss_end)）
    .long _start              # entry_addr
header_end:

.section .text.boot
.code32
.global _start
_start:
    cli
    mov     %eax, multiboot_magic
    mov     %ebx, multiboot_info

    # 检查 CPUID 和长模式支持
    call    check_cpuid
    call    check_long_mode

    # 设置临时页表 (PML4, PDPT, PD)
    call    setup_paging

    # 启用 PAE 和 PGE
    mov     %cr4, %eax
    or      $0xa0, %eax        # PAE + PGE
    mov     %eax, %cr4

    # 加载 PML4 地址到 CR3
    mov     $pml4_table, %eax
    mov     %eax, %cr3

    # 启用长模式 (EFER.LME)
    mov     $0xC0000080, %ecx
    rdmsr
    or      $0x100, %eax
    wrmsr

    # 启用分页 (CR0.PG)
    mov     %cr0, %eax
    or      $0x80000000, %eax
    mov     %eax, %cr0

    # 加载 64 位 GDT
    lgdt    (gdt64_desc)

    # 远跳转进入长模式
    ljmp    $0x08, $long_mode_start

.code64
long_mode_start:
    # 设置段寄存器
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss

    # 设置栈
    mov     $stack_top, %rsp
    mov     $stack_top, %rbp

    # 调用 C 内核入口
    mov     multiboot_magic, %edi
    mov     multiboot_info, %esi
    call    kernel_main

    cli
    hlt
    jmp     .

# ---------- 辅助函数 ----------
# 构建修复：这些辅助函数位于 .code64 之后，必须切回 .code32
#（它们用 32 位寄存器与 (%rsp) 栈操作；原文件漏掉 .code32，
#  导致 as --64 按 64 位语义汇编，报 "operand size mismatch for pop"）
.code32
check_cpuid:
    pushf
    pushf
    xorl    $0x200000, (%esp)
    popf
    pushf
    pop     %eax
    xor     (%esp), %eax
    popf
    test    $0x200000, %eax
    jz      .no_cpuid
    ret
.no_cpuid:
    mov     $0x4f, %al
    mov     $'C',  %ah
    jmp     error

check_long_mode:
    mov     $0x80000000, %eax
    cpuid
    cmp     $0x80000001, %eax
    jb      .no_long_mode
    mov     $0x80000001, %eax
    cpuid
    test    $0x20000000, %edx
    jz      .no_long_mode
    ret
.no_long_mode:
    mov     $0x4f, %al
    mov     $'L',  %ah
    jmp     error

setup_paging:
    # PML4[0] -> PDPT
    mov     $pml4_table, %edi
    mov     $pdpt_table, %eax
    or      $0x3, %eax          # present + writable
    mov     %eax, (%edi)

    # PDPT[0..3] -> pd_table 的 4 个 4KB 页（共 4GB 地址空间）
    mov     $pdpt_table, %edi
    mov     $pd_table, %eax
    or      $0x3, %eax
    mov     %eax, (%edi)          # PDPT[0]
    add     $8, %edi
    add     $0x1000, %eax
    mov     %eax, (%edi)          # PDPT[1]
    add     $8, %edi
    add     $0x1000, %eax
    mov     %eax, (%edi)          # PDPT[2]
    add     $8, %edi
    add     $0x1000, %eax
    mov     %eax, (%edi)          # PDPT[3]

    # PD: 2048 个 2MB 大页 = 0-4GB 恒等映射
    # （构建修复：由 512 项/1GB 扩展到 2048 项/4GB —— GUI 帧缓冲
    #   Bochs VBE LFB 位于 0xE0000000（3.5GB），必须映射到 4GB）
    mov     $pd_table, %edi
    mov     $0x83, %eax         # huge page, present, writable
    xor     %ebx, %ebx          # 物理地址 0
    mov     $2048, %ecx
1:
    # 构建修复：条目 = 物理基址 | 0x83 标志。
    # 原代码只存 %ebx（纯地址），0x83 的 present/writable/PS 标志全丢，
    # 导致所有 PD 条目 not-present，开分页后第一条指令就 #PF（三连故障）。
    lea     (%ebx,%eax), %edx
    mov     %edx, (%edi)
    add     $0x200000, %ebx     # 2MB
    add     $8, %edi
    loop    1b
    ret

error:
    mov     $0xb8000, %edi
    mov     %ax, (%edi)
    cli
    hlt

# ---------- 数据 ----------
.section .data
.align 4096
pml4_table:
    .space 4096
pdpt_table:
    .space 4096
pd_table:
    .space 16384               # 构建修复：2048 项 × 8B = 4GB 映射

gdt64:
    .quad 0x0000000000000000   # null
    .quad 0x0020980000000000   # 64-bit code
    .quad 0x0000920000000000   # data
gdt64_desc:
    .word . - gdt64 - 1
    .quad gdt64

multiboot_magic:
    .long 0
multiboot_info:
    .long 0

.section .bss
.align 16
stack_bottom:
    .space 16384               # 16KB 内核栈
stack_top:

# ================================================================
# 构建修复：PVH (Xen) ELF note — QEMU 8.2 的 -kernel 加载 ELF 时
# 要求该 note（否则报 "Error loading uncompressed kernel without
# PVH ELF Note"）。QEMU 读到后按 ELF 程序头加载，并以 32 位保护
# 模式跳入 _start（EAX/EBX 内容本内核不使用，_start 自带分页初始化）。
# 保留前面的 multiboot v1 头：GRUB/裸二进制路径仍然可用。
# ================================================================
.section .note.Xen, "a", @note
.align 4
    .long 4                  # namesz: "Xen\0"
    .long 4                  # descsz
    .long 18                 # type: XEN_ELFNOTE_PHYS32_ENTRY
    .asciz "Xen"             # name
    .align 4
    .long _start             # desc: 32 位物理入口地址