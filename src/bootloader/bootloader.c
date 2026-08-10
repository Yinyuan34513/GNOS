/*
 * bootloader.c — gnos UEFI bootloader (x86-64, gnu-efi). (GPLv2)
 *
 * Responsibilities:
 *   1. Find the volume we were loaded from and read \gnos\GNOSKr.elf
 *      (the 64-bit kernel) and \gnos\initrd.img (a FAT image holding
 *      init.elf) into memory.
 *   2. Parse GNOSKr.elf and copy its PT_LOAD segments to their (identity
 *      mapped) load addresses; record the entry point.
 *   3. Capture a GOP framebuffer and a copy of the UEFI memory map into a
 *      bootinfo structure at GNUCOS_BOOTINFO_ADDR.
 *   4. Build identity page tables + a 64-bit GDT, enable long-mode paging
 *      (UEFI is already in long mode), call ExitBootServices(), and jump
 *      to the kernel entry with RDI = bootinfo address.
 */
#include <efi.h>
#include <efilib.h>
#include <efiapi.h>
#include <stdint.h>

#include "bootinfo.h"

/* early trace via QEMU debugcon (port 0xE9), works even under UEFI */
static void bout(uint8_t v) { asm volatile("outb %0,%1" :: "a"(v), "Nd"((uint16_t)0xE9)); }
static void bputs(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') bout('\r');
        bout((uint8_t)*s);
    }
}
#define BTRACE(s) bputs("BL:" s "\r\n")

/* ---------------- ELF64 structures ---------------- */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

#define ELF_ET_EXEC   2
#define ELF_EM_X86_64 62
#define ELF_PT_LOAD   1

/* ---------------- file helpers ---------------- */
static EFI_STATUS
open_file(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, CHAR16 *path, EFI_FILE **out)
{
    EFI_LOADED_IMAGE_PROTOCOL        *loaded = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *fs     = NULL;
    EFI_FILE                         *root   = NULL;
    EFI_STATUS r;

    r = uefi_call_wrapper(st->BootServices->HandleProtocol, 3,
                          image, &gEfiLoadedImageProtocolGuid,
                          (VOID **)&loaded);
    if (EFI_ERROR(r))
        return r;

    r = uefi_call_wrapper(st->BootServices->HandleProtocol, 3,
                          loaded->DeviceHandle,
                          &gEfiSimpleFileSystemProtocolGuid, (VOID **)&fs);
    if (EFI_ERROR(r))
        return r;

    r = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
    if (EFI_ERROR(r))
        return r;

    return uefi_call_wrapper(root->Open, 5, root, out, path,
                             EFI_FILE_MODE_READ, 0);
}

static VOID *
read_file(EFI_SYSTEM_TABLE *st, EFI_FILE *f, UINTN *size_out)
{
    EFI_FILE_INFO *info = NULL;
    UINTN          infosz = 0;
    UINTN          size   = 0;
    VOID          *buf;
    EFI_STATUS     r;

    r = uefi_call_wrapper(f->GetInfo, 4, f,
                          &gEfiFileInfoGuid, &infosz, NULL);
    if (r != EFI_BUFFER_TOO_SMALL || !infosz)
        return NULL;

    info = AllocateZeroPool(infosz);
    if (!info)
        return NULL;
    r = uefi_call_wrapper(f->GetInfo, 4, f,
                          &gEfiFileInfoGuid, &infosz, info);
    if (EFI_ERROR(r)) {
        FreePool(info);
        return NULL;
    }
    size = (UINTN)info->FileSize;
    FreePool(info);
    if (!size)
        return NULL;

    buf = AllocateZeroPool(size);
    if (!buf)
        return NULL;

    r = uefi_call_wrapper(f->Read, 3, f, &size, buf);
    if (EFI_ERROR(r)) {
        FreePool(buf);
        return NULL;
    }
    *size_out = size;
    return buf;
}

/* Copy one PT_LOAD segment to its p_vaddr (identity mapped). */
static EFI_STATUS
map_segment(EFI_SYSTEM_TABLE *st, const elf64_phdr_t *ph,
            const VOID *file, UINTN filesize)
{
    uint64_t target = ph->p_vaddr;
    uint64_t avail  = ph->p_memsz;
    const char *src = (const char *)file + ph->p_offset;
    char *dst = (char *)(UINTN)target;
    uint64_t n = ph->p_filesz;

    if (ph->p_offset + ph->p_filesz > filesize) {
        Print(L"  segment past EOF\r\n");
        return EFI_LOAD_ERROR;
    }
    while (n--)
        *dst++ = *src++;
    while (avail-- > ph->p_filesz)
        *dst++ = 0;
    (void)st;
    return EFI_SUCCESS;
}

static uint64_t
map_kernel(EFI_SYSTEM_TABLE *st, const VOID *file, UINTN filesize)
{
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)file;
    UINT16 i;

    if (filesize < sizeof(*eh) ||
        __builtin_memcmp(eh->e_ident, "\177ELF", 4) != 0) {
        Print(L"  not an ELF\r\n");
        return 0;
    }
    if (eh->e_type != ELF_ET_EXEC ||
        eh->e_machine != ELF_EM_X86_64 ||
        eh->e_phentsize < sizeof(elf64_phdr_t)) {
        Print(L"  not an x86-64 ET_EXEC\r\n");
        return 0;
    }
    for (i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph =
            (const elf64_phdr_t *)((const char *)file + eh->e_phoff +
                                   (UINTN)i * eh->e_phentsize);
        if (ph->p_type != ELF_PT_LOAD)
            continue;
        if (EFI_ERROR(map_segment(st, ph, file, filesize)))
            return 0;
    }
    return eh->e_entry;
}

/* ---------------- paging + GDT ---------------- */
/* Identity-map the first 4 GiB with 2 MiB pages.  Returns CR3 (pml4 phys). */
static EFI_PHYSICAL_ADDRESS
setup_identity_paging(EFI_SYSTEM_TABLE *st)
{
    EFI_STATUS r;
    EFI_PHYSICAL_ADDRESS pages = 0;
    r = st->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData, 6, &pages);
    if (EFI_ERROR(r))
        return 0;

    uint64_t *pml4 = (uint64_t *)(UINTN)pages;
    uint64_t *pdpt = (uint64_t *)(UINTN)(pages + 0x1000);
    uint64_t *pd   = (uint64_t *)(UINTN)(pages + 0x2000);
    for (UINTN i = 0; i < 6 * 512; i++)
        ((uint64_t *)(UINTN)pages)[i] = 0;

    pml4[0] = (UINTN)pdpt | 0x3;                 /* present | rw */
    for (int p = 0; p < 4; p++) {
        pdpt[p] = (UINTN)(pd + (UINTN)p * 512) | 0x3;
        for (int i = 0; i < 512; i++) {
            uint64_t addr = (uint64_t)p * 0x40000000ULL +
                            (uint64_t)i * 0x200000ULL;
            pd[p * 512 + i] = addr | 0x83;       /* present | rw | PS(2MB) */
        }
    }
    return pages;
}

static VOID
setup_gdt(EFI_SYSTEM_TABLE *st)
{
    EFI_STATUS r;
    EFI_PHYSICAL_ADDRESS gp = 0;
    r = st->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &gp);
    if (EFI_ERROR(r))
        return;

    uint64_t *gdt = (uint64_t *)(UINTN)gp;
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;   /* 64-bit code */
    gdt[2] = 0x00CF92000000FFFFULL;   /* 64-bit data */

    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr;
    gdtr.limit = 3 * 8 - 1;
    gdtr.base  = (uint64_t)(UINTN)gdt;

    asm volatile("lgdt %0" :: "m"(gdtr));
    uint16_t ds = 0x10;
    asm volatile("mov %0, %%ds; mov %0, %%es; mov %0, %%ss; "
                 "mov %0, %%fs; mov %0, %%gs" :: "r"(ds));
}

static BOOLEAN
grab_gop(EFI_SYSTEM_TABLE *st, bootinfo_t *bi)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS r;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;

    r = uefi_call_wrapper(st->BootServices->LocateProtocol, 3,
                          &guid, NULL, (VOID **)&gop);
    if (EFI_ERROR(r) || !gop || !gop->Mode->Info)
        return FALSE;

    info          = gop->Mode->Info;
    bi->fb_addr   = gop->Mode->FrameBufferBase;
    bi->fb_width  = info->HorizontalResolution;
    bi->fb_height = info->VerticalResolution;
    bi->fb_pitch  = gop->Mode->FrameBufferSize /
                    info->VerticalResolution;
    bi->fb_bpp    = (UINT32)(bi->fb_pitch * 8 / info->HorizontalResolution);
    bi->fb_type   = GNUCOS_FB_RGB;
    return TRUE;
}

/* ---------------- entry ---------------- */
EFI_STATUS
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    EFI_FILE  *f = NULL;
    VOID      *buf = NULL;
    UINTN      len = 0;
    bootinfo_t *bi = (bootinfo_t *)GNUCOS_BOOTINFO_ADDR;
    uint64_t   entry;

    InitializeLib(image, st);
    BTRACE("initlib");
    Print(L"gnos bootloader 0.1 (GPLv2)\r\n");

    EFI_PHYSICAL_ADDRESS cr3 = setup_identity_paging(st);
    if (!cr3) {
        Print(L"paging setup failed\r\n");
        return EFI_LOAD_ERROR;
    }
    BTRACE("paging");
    setup_gdt(st);
    BTRACE("gdt");
    asm volatile("mov %0, %%cr3" :: "r"(cr3));
    BTRACE("cr3");

    SetMem(bi, sizeof(*bi), 0);
    bi->magic = GNUCOS_BOOTINFO_MAGIC;
    if (!grab_gop(st, bi))
        bi->fb_addr = 0;
    BTRACE("gop");

    /* 1. load the kernel */
    BTRACE("openkern");
    if (EFI_ERROR(open_file(image, st, L"\\gnos\\GNOSKr.elf", &f))) {
        Print(L"failed to open \\gnos\\GNOSKr.elf\r\n");
        return EFI_LOAD_ERROR;
    }
    buf = read_file(st, f, &len);
    if (!buf) {
        Print(L"failed to read GNOSKr.elf\r\n");
        return EFI_LOAD_ERROR;
    }
    Print(L"loading GNOSKr.elf\r\n");
    entry = map_kernel(st, buf, len);
    if (!entry) {
        Print(L"GNOSKr.elf has no usable entry\r\n");
        return EFI_LOAD_ERROR;
    }
    bi->kernel_entry = entry;
    BTRACE("kernmapped");

    /* 2. load the initrd (FAT image containing init.elf) */
    {
        EFI_FILE *f2 = NULL;
        if (!EFI_ERROR(open_file(image, st, L"\\gnos\\initrd.img", &f2))) {
            VOID *ibuf; UINTN ilen;
            ibuf = read_file(st, f2, &ilen);
            if (ibuf) {
                bi->initrd_addr = (uint64_t)(UINTN)ibuf;
                bi->initrd_size = (uint64_t)ilen;
                Print(L"initrd loaded\r\n");
            }
        } else {
            Print(L"warning: \\gnos\\initrd.img not found\r\n");
        }
    }

    /* 3. memory map snapshot (before ExitBootServices) */
    {
        UINTN msize = 0, dsize = 0, key = 0;
        UINT32 dver = 0;
        st->BootServices->GetMemoryMap(&msize, NULL, &key, &dsize, &dver);
        msize += 8192;
        VOID *map = AllocatePool(msize);
        if (map) {
            st->BootServices->GetMemoryMap(&msize, map, &key, &dsize, &dver);
            bi->mmap_addr      = GNUCOS_BOOTINFO_ADDR + sizeof(bootinfo_t);
            bi->mmap_size      = msize;
            bi->mmap_desc_size = dsize;
            bi->mmap_ver       = dver;
            uint64_t *dst = (uint64_t *)(UINTN)bi->mmap_addr;
            uint64_t *src = (uint64_t *)map;
            for (UINTN i = 0; i < msize / 8; i++)
                dst[i] = src[i];
            FreePool(map);
        }

        /* 4. leave firmware behind */
        BTRACE("prexbs");
        EFI_STATUS r = st->BootServices->ExitBootServices(image, key);
        if (EFI_ERROR(r)) {
            Print(L"ExitBootServices failed\r\n");
            return r;
        }
        BTRACE("postxbs");
    }

    /* 5. jump to the kernel (RDI = bootinfo) */
    BTRACE("jump");
    typedef void (*kf)(bootinfo_t *);
    kf k = (kf)(UINTN)entry;
    k(bi);

    for (;;)
        asm volatile("cli; hlt");
}
