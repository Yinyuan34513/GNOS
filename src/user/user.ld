/* src/user/user.ld — every user program is linked at 0x400000.
 *
 * They can all share one address because they never share an address space:
 * each process gets its own set of page tables, so "0x400000" means something
 * different in each of them.  That is the whole point of per-process paging,
 * and it is what lets the loader stay this simple -- no relocation, no PIE.
 *
 * We declare explicit program headers so linked images carry a PT_TLS (musl
 * reads it from auxv to lay out thread-local storage) and a PT_GNU_STACK.
 * .tdata belongs to both the data LOAD segment (so its bytes get mapped into
 * memory) and the TLS segment (so musl can find the template), which is why
 * it lists two :phdr qualifiers.
 */
ENTRY(_start)

PHDRS {
    /* FILEHDR/PHDRS pull the ELF header and the program header table into the
     * first LOAD segment.  Without them the table sits at file offset 64 while
     * the segment starts at 0x1000, so it is never mapped and the kernel has
     * no runtime address to report in AT_PHDR -- which musl needs to find
     * PT_TLS.  The cost is that the image now starts at 0x400000 + SIZEOF_HEADERS
     * rather than exactly 0x400000. */
    text   PT_LOAD FILEHDR PHDRS FLAGS(5);  /* R + X */
    rodata PT_LOAD FLAGS(4);           /* R */
    data   PT_LOAD FLAGS(6);           /* R + W */
    tls    PT_TLS  FLAGS(6);           /* thread-local template */
    stack  PT_GNU_STACK FLAGS(6);
}

SECTIONS
{
    . = 0x400000 + SIZEOF_HEADERS;

    .text : {
        *(.text.start)
        *(.text*)
    } :text

    /* crti.o opens .init/.fini and crtn.o closes them; the halves must end up
     * adjacent and in link order or the _init/_fini they form is not a valid
     * function.  Naming them explicitly also keeps them out of the linker's
     * orphan-placement heuristics, which are free to put them anywhere. */
    .init : { KEEP(*(.init)) } :text
    .fini : { KEEP(*(.fini)) } :text

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata*)
    } :rodata

    . = ALIGN(0x1000);
    .data : {
        *(.data.rel.ro*)
        *(.data*)
    } :data

    /*
     * C++-style constructors and anything marked __attribute__((constructor)).
     * musl's __libc_start_main walks from __init_array_start to
     * __init_array_end, so those two symbols must bracket the section exactly.
     * Without this block the sections are orphans, both symbols resolve to
     * zero as undefined weaks, and every constructor is silently skipped --
     * which is the kind of bug that shows up as unexplained NULLs much later.
     * KEEP() stops --gc-sections from removing entries nothing appears to call.
     */
    . = ALIGN(8);
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } :data

    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :data

    .tdata : {
        *(.tdata .tdata.*)
    } :data :tls

    .tbss : {
        *(.tbss .tbss.* .tcommon)
    } :tls

    .got : {
        *(.got .got.plt)
    } :data

    .bss : {
        *(COMMON)
        *(.bss*)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
