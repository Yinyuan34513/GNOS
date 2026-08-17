/*
 * cjkfont.S — the CJK bitmap font, stapled into the kernel image. (GPLv2)
 *
 * The glyphs come from GNU Unifont (GPLv2+ with the font embedding
 * exception), converted by tools/mkcjkfont.py into the flat format described
 * there.  A megabyte of bitmaps is not something to hand to the C compiler as
 * an initialiser -- thirty thousand braced rows would take longer to compile
 * than the rest of the kernel put together -- so the binary is included
 * verbatim by the assembler and parsed at boot by cjkfont.c.
 *
 * It lives in the kernel rather than the initrd because the console has to be
 * able to print before there is a filesystem to load a font from.
 */
	.section .rodata
	.balign 16

	.globl cjkfont_blob
cjkfont_blob:
	.incbin "src/kernel/driver/cjkfont.bin"
	.globl cjkfont_blob_end
cjkfont_blob_end:

	.balign 8
	.globl cjkfont_blob_size
cjkfont_blob_size:
	.quad cjkfont_blob_end - cjkfont_blob

	/* Nothing here is executable; say so, or the linker warns about a
	 * section with no note and marks the stack executable. */
	.section .note.GNU-stack,"",@progbits
