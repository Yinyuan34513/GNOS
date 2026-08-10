/* src/init/linker.ld — init.elf: 64-bit, linked/loaded at 0x600000. */
ENTRY(_start)

SECTIONS
{
    . = 0x600000;

    .text : {
        *(.text)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(COMMON)
        *(.bss)
    }
}
