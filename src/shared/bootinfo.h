/*
 * bootinfo.h — shared between UEFI bootloader and kernel. (GPLv2)
 *
 * The bootloader lays out one of these at a fixed physical address
 * (GNUCOS_BOOTINFO_ADDR) just before handing control to the kernel.
 */
#ifndef GNUCOS_BOOTINFO_H
#define GNUCOS_BOOTINFO_H

#include <stdint.h>

#define GNUCOS_BOOTINFO_MAGIC 0x474E5543464F4F42ULL /* "BOOFUNCG" */
#define GNUCOS_BOOTINFO_ADDR  0x8000

#define GNUCOS_FB_UNKNOWN     0
#define GNUCOS_FB_RGB         1

typedef struct {
    uint64_t magic;                 /* GNUCOS_BOOTINFO_MAGIC */
    uint64_t kernel_entry;          /* physical entry point  */

    /* memory map (copied by bootloader) */
    uint64_t mmap_addr;             /* -> EFI memory descriptors */
    uint64_t mmap_size;             /* bytes */
    uint64_t mmap_desc_size;        /* per-entry size, UINTN aligned */
    uint64_t mmap_ver;              /* MemoryDescriptorVersion */

    /* framebuffer set up via GOP */
    uint64_t fb_addr;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;              /* bytes / scanline */
    uint32_t fb_bpp;                /* bits per pixel */
    uint32_t fb_type;               /* GNUCOS_FB_* */
    uint32_t rsdp_addr;             /* ACPI RSDP, 0 if none */

    /* firmware text columns/rows if framebuffer unusable */
    uint32_t vga_cols;
    uint32_t vga_rows;

    /* initrd: a RAM-backed FAT image (read by the kernel's FS driver) */
    uint64_t initrd_addr;             /* physical address of FAT image */
    uint64_t initrd_size;             /* bytes */
} __attribute__((packed)) bootinfo_t;

#endif