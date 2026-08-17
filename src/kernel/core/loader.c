/*
 * loader.c — ELF64 / a.out image loader. (GPLv2)
 *
 * Pure freestanding code, no allocations of its own beyond page frames.  Each
 * loadable segment is backed with fresh zeroed frames in the target address
 * space and then filled in through the direct map, so the image is complete
 * before anybody switches to it.
 */
#include <stddef.h>
#include <stdint.h>

#include "loader.h"
#include "vmm.h"
#include "debugcon.h"

/* ----------------------------- ELF64 ----------------------------- */
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

#define ELF_PT_LOAD   1
#define ELF_ET_EXEC   2
#define ELF_EM_X86_64 62

#define ELF_PF_X      1
#define ELF_PF_W      2

/* Nothing user-space is allowed to live at or above this line. */
#define USER_VA_LIMIT 0x0000800000000000ULL

static int is_elf(const uint8_t *img)
{
    return img[0] == 0x7F && img[1] == 'E' && img[2] == 'L' && img[3] == 'F';
}

static int load_elf(addrspace_t *as, const uint8_t *img, uint32_t size,
                    uint64_t *entry, uint64_t *phdr, uint16_t *phnum)
{
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)img;

    if (size < sizeof(*eh))
        return -1;
    if (eh->e_type != ELF_ET_EXEC)
        return -1;
    if (eh->e_machine != ELF_EM_X86_64)
        return -1;
    if (eh->e_phoff == 0 || eh->e_phnum == 0)
        return -1;
    if (eh->e_phentsize < sizeof(elf64_phdr_t))
        return -1;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size)
        return -1;

    /* The program header table normally lives inside the first loadable
     * segment, so its virtual address is that segment's vaddr plus the
     * offset of the table within the file.  musl's _start needs AT_PHDR. */
    uint64_t phdr_vaddr = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph =
            (const elf64_phdr_t *)(img + eh->e_phoff +
                                   (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != ELF_PT_LOAD)
            continue;
        if (ph->p_filesz > ph->p_memsz)
            return -1;
        if (ph->p_offset + ph->p_filesz > size)
            return -1;
        if (ph->p_vaddr >= USER_VA_LIMIT ||
            ph->p_vaddr + ph->p_memsz > USER_VA_LIMIT)
            return -1;

        if (!phdr_vaddr && eh->e_phoff >= ph->p_offset &&
            eh->e_phoff < ph->p_offset + ph->p_filesz)
            phdr_vaddr = ph->p_vaddr + (eh->e_phoff - ph->p_offset);

        /* We map every segment writable: the loader has to be able to fill
         * it in, and without per-segment W^X enforcement there is nothing to
         * be gained by taking the permission away again afterwards. */
        unsigned flags = VM_USER | VM_WRITE;
        if (ph->p_flags & ELF_PF_X)
            flags |= VM_EXEC;

        if (!vmm_alloc_range(as, ph->p_vaddr, ph->p_memsz, flags))
            return -1;

        /* Frames come out of the allocator zeroed, so .bss needs no work. */
        if (ph->p_filesz &&
            !vmm_copy_to_user(as, ph->p_vaddr, img + ph->p_offset, ph->p_filesz))
            return -1;
    }

    if (eh->e_entry == 0 || eh->e_entry >= USER_VA_LIMIT)
        return -1;

    *entry = eh->e_entry;
    /* Report the table only if it actually landed in the address space.  A
     * bare phdr_vaddr of 0 paired with a non-zero count is what used to make
     * musl dereference NULL in __init_tls. */
    if (phdr)
        *phdr = phdr_vaddr;
    if (phnum)
        *phnum = phdr_vaddr ? eh->e_phnum : 0;
    return 1;
}

/* ----------------------------- a.out ----------------------------- *
 * Classic a.out with 8-byte fields.  The magic lives in the low 24 bits of
 * the first word.  OMAGIC puts text and data right after the header; ZMAGIC
 * page-aligns the text at file offset 0x1000.
 * ----------------------------------------------------------------- */
typedef struct {
    uint64_t a_magic;
    uint64_t a_text;
    uint64_t a_data;
    uint64_t a_bss;
    uint64_t a_syms;
    uint64_t a_entry;
    uint64_t a_trsize;
    uint64_t a_drsize;
} __attribute__((packed)) aout_exec_t;

#define AOUT_OMAGIC 0407
#define AOUT_NMAGIC 0410
#define AOUT_ZMAGIC 0411
#define AOUT_BASE   0x800000ULL

static uint32_t aout_magic(const uint8_t *img)
{
    return (uint32_t)img[0] | ((uint32_t)img[1] << 8) | ((uint32_t)img[2] << 16);
}

static int is_aout(const uint8_t *img, uint32_t size)
{
    if (size < sizeof(aout_exec_t))
        return 0;
    uint32_t m = aout_magic(img);
    return m == AOUT_OMAGIC || m == AOUT_NMAGIC || m == AOUT_ZMAGIC;
}

static int load_aout(addrspace_t *as, const uint8_t *img, uint32_t size,
                     uint64_t *entry)
{
    const aout_exec_t *e = (const aout_exec_t *)img;
    uint64_t text_off = (aout_magic(img) == AOUT_ZMAGIC)
                        ? 0x1000 : sizeof(aout_exec_t);
    uint64_t data_off = text_off + e->a_text;

    if (data_off > size)
        return -2;

    uint64_t total = e->a_text + e->a_data + e->a_bss;
    if (total == 0 || AOUT_BASE + total > USER_VA_LIMIT)
        return -2;

    if (!vmm_alloc_range(as, AOUT_BASE, total, VM_USER | VM_WRITE | VM_EXEC))
        return -2;

    if (e->a_text &&
        !vmm_copy_to_user(as, AOUT_BASE, img + text_off, e->a_text))
        return -2;

    uint64_t dsize = e->a_data;
    if (data_off + dsize > size)
        dsize = size - data_off;
    if (dsize &&
        !vmm_copy_to_user(as, AOUT_BASE + e->a_text, img + data_off, dsize))
        return -2;

    *entry = AOUT_BASE + e->a_entry;
    return 2;
}

/* --------------------------- dispatch --------------------------- */
int load_executable(addrspace_t *as, const uint8_t *img, uint32_t size,
                    uint64_t *entry, uint64_t *phdr, uint16_t *phnum)
{
    if (phdr)
        *phdr = 0;
    if (phnum)
        *phnum = 0;
    if (size < 4)
        return 0;
    if (is_elf(img))
        return load_elf(as, img, size, entry, phdr, phnum);
    if (is_aout(img, size))
        return load_aout(as, img, size, entry);
    return 0;
}
