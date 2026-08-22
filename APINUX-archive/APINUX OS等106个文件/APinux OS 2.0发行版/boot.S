// ================================================================
// boot.S — APinux OS 启动入口 (ARMv8-A)
// 功能：设置异常级别、MMU、栈，跳转到 kernel_main
// ================================================================
.section .text.boot
.global _start
_start:
    // 当前运行在 EL2 或 EL1，目标：切换到 EL1
    mrs     x0, CurrentEL
    cmp     x0, #(2 << 2)          // EL2?
    b.eq    1f
    cmp     x0, #(1 << 2)          // EL1?
    b.eq    2f
    b       .

1:  // 从 EL2 降级到 EL1
    mov     x0, #(1 << 31)         // EL1h (使用 SP_EL1)
    msr     spsr_el2, x0
    adr     x0, 2f
    msr     elr_el2, x0
    eret

2:  // 设置栈指针 (EL1)
    ldr     x0, =_stack_top
    mov     sp, x0

    // 清零 BSS
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
3:  str     xzr, [x0], #8
    cmp     x0, x1
    b.lo    3b

    // 设置异常向量表
    ldr     x0, =_exception_vector
    msr     vbar_el1, x0

    // 启用 MMU (假设页表已由 Bootloader 初始化)
    ldr     x0, =_ttbr0_base
    msr     ttbr0_el1, x0
    ldr     x0, =_ttbr1_base
    msr     ttbr1_el1, x0

    // 配置 MAIR, TCR
    ldr     x0, =0x44                   // 普通内存属性
    msr     mair_el1, x0
    ldr     x0, =0x00000000103500       // TCR_EL1 配置
    msr     tcr_el1, x0

    // 启用 MMU
    mrs     x0, sctlr_el1
    orr     x0, x0, #1                  // M bit
    msr     sctlr_el1, x0
    isb

    // 跳转到 C 内核入口
    adr     x0, _dtb                   // 设备树地址 (由 Bootloader 传入)
    bl      kernel_main
    b       .

.section .bss
.align 12
_stack:
    .space 65536                       // 64KB 内核栈
_stack_top:

__bss_start:
    .space 4096
__bss_end:

_ttbr0_base:
    .quad 0x80000000
_ttbr1_base:
    .quad 0x80000000
_dtb:
    .quad 0

.section .rodata
_exception_vector:
    .space 2048                       // 异常向量表 (简化)