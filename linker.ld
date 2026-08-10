/* linker.ld — GnuKr.elf: 64-bit PIE kernel (relocated by Limine). */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kernel_entry)

PHDRS
{
    limine_requests PT_LOAD FLAGS(4);   /* read-only */
    text            PT_LOAD FLAGS(5);   /* rx */
    rodata          PT_LOAD FLAGS(4);   /* r  */
    data            PT_LOAD FLAGS(6);   /* rw */
    dynamic         PT_DYNAMIC FLAGS(4);
}

SECTIONS
{
    . = 0;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .text : {
        *(.text)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .rodata : {
        *(.rodata*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .dynamic : {
        *(.dynamic)
    } :rodata :dynamic

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data : {
        *(.data)
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .bss : {
        _bss_start = .;
        *(COMMON)
        *(.bss)
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    _kernel_end = .;
}
