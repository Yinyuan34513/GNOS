/* ================================================================
 * linker.ld — APinux OS x86_64 链接脚本（构建修复版 v2）
 * 布局：.multiboot_header 与 .text.boot/.text 合并为同一个输出段
 *（分开时 ld 把 multiboot 头文件偏移排到 .bss 之后，导致
 *  objcopy -O binary 的产物开头不是 magic，QEMU 报 invalid kernel
 *  header）。合并后文件布局 = 内存布局，二进制首字节即 0x1BADB002。
 * _load_end / _end 供 multiboot 头做 BSS 清零区间。
 * ================================================================ */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .text : ALIGN(4096) {
        KEEP(*(.multiboot_header))
        *(.text.boot)
        *(.text*)
    }

    .note : {
        *(.note.Xen)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }
    _load_end = .;

    .bss : {
        *(COMMON)
        *(.bss*)
    }

    . = ALIGN(4096);
    _end = .;
}
