/*
 * module_elf.c -- ELF validation and x86-64 relocation for loadable
 * kernel modules.  (GPLv2)
 *
 * Adapted from Uinxed-Kernel's kernel/module/elf.c to GNOS's own errno
 * constants and coding style: same on-disk checks and the same relocation
 * math, but no dependency on any external elf headers.
 */
#include "module_elf.h"
#include "vfs.h"

#define ELF64_EI_MAG0 0
#define ELF64_EI_CLASS 4
#define ELF64_EI_DATA 5
#define ELF64_EI_VERSION 6
#define ELF64_ELFCLASS64 2
#define ELF64_ELFDATA2LSB 1
#define ELF64_EV_CURRENT 1

/* R_X86_64_* relocation types the relocatable-object linker emits. */
#define R_X86_64_NONE   0
#define R_X86_64_64     1
#define R_X86_64_PC32   2
#define R_X86_64_PLT32  4
#define R_X86_64_32     10
#define R_X86_64_32S    11
#define R_X86_64_16     12
#define R_X86_64_PC16   13
#define R_X86_64_8      14
#define R_X86_64_PC8    15
#define R_X86_64_PC64   24
#define R_X86_64_SIZE32 32
#define R_X86_64_SIZE64 33

static int range_valid(size_t offset, size_t length, size_t total)
{
    return offset <= total && length <= total - offset;
}

static int power_of_two(uint64_t value)
{
    return !value || !(value & (value - 1));
}

int module_elf_validate(const void *image, size_t size, module_elf_view_t *view)
{
    if (!image || !view || size < sizeof(elf64_ehdr_t))
        return -E_NOEXEC;

    const elf64_ehdr_t *header = (const elf64_ehdr_t *)image;
    if (header->e_ident[ELF64_EI_MAG0] != 0x7f || header->e_ident[1] != 'E' ||
        header->e_ident[2] != 'L' || header->e_ident[3] != 'F' ||
        header->e_ident[ELF64_EI_CLASS] != ELF64_ELFCLASS64 ||
        header->e_ident[ELF64_EI_DATA] != ELF64_ELFDATA2LSB ||
        header->e_ident[ELF64_EI_VERSION] != ELF64_EV_CURRENT ||
        header->e_type != ELF64_ET_REL ||
        header->e_machine != ELF64_EM_X86_64 ||
        header->e_version != ELF64_EV_CURRENT ||
        header->e_ehsize != sizeof(elf64_ehdr_t) ||
        header->e_shentsize != sizeof(elf64_shdr_t))
        return -E_NOEXEC;

    if (!header->e_shoff || !range_valid(header->e_shoff, sizeof(elf64_shdr_t), size))
        return -E_NOEXEC;
    const elf64_shdr_t *sections = (const elf64_shdr_t *)((const uint8_t *)image + header->e_shoff);
    size_t count = header->e_shnum ? header->e_shnum : sections[0].sh_size;
    size_t names = header->e_shstrndx == ELF64_SHN_XINDEX ? sections[0].sh_link : header->e_shstrndx;
    if (!count || count > 0xffff ||
        !range_valid(header->e_shoff, count * sizeof(elf64_shdr_t), size))
        return -E_NOEXEC;
    if (names != ELF64_SHN_UNDEF && names >= count)
        return -E_NOEXEC;

    for (size_t index = 0; index < count; index++) {
        const elf64_shdr_t *section = &sections[index];
        if (!power_of_two(section->sh_addralign))
            return -E_NOEXEC;
        if (section->sh_type != ELF64_SHT_NOBITS &&
            !range_valid(section->sh_offset, section->sh_size, size))
            return -E_NOEXEC;
        if (section->sh_type == ELF64_SHT_SYMTAB &&
            (section->sh_entsize != sizeof(elf64_sym_t) ||
             section->sh_link >= count ||
             section->sh_size % sizeof(elf64_sym_t)))
            return -E_NOEXEC;
        if (section->sh_type == ELF64_SHT_RELA &&
            (section->sh_entsize != sizeof(elf64_rela_t) ||
             section->sh_link >= count || section->sh_info >= count ||
             section->sh_size % sizeof(elf64_rela_t)))
            return -E_NOEXEC;
    }
    if (names != ELF64_SHN_UNDEF && sections[names].sh_type != ELF64_SHT_STRTAB)
        return -E_NOEXEC;

    view->image              = image;
    view->size               = size;
    view->header             = header;
    view->sections           = sections;
    view->section_count      = count;
    view->section_name_index = names;
    return 0;
}

static int store_signed(void *location, __int128 value, unsigned int bits)
{
    __int128 minimum = -((__int128)1 << (bits - 1));
    __int128 maximum = ((__int128)1 << (bits - 1)) - 1;
    if (value < minimum || value > maximum)
        return -E_NOEXEC;
    if (bits == 8)
        *(int8_t *)location = (int8_t)value;
    else if (bits == 16)
        *(int16_t *)location = (int16_t)value;
    else if (bits == 32)
        *(int32_t *)location = (int32_t)value;
    else
        *(int64_t *)location = (int64_t)value;
    return 0;
}

static int store_unsigned(void *location, __int128 value, unsigned int bits)
{
    __int128 maximum = ((__int128)1 << bits) - 1;
    if (value < 0 || value > maximum)
        return -E_NOEXEC;
    if (bits == 8)
        *(uint8_t *)location = (uint8_t)value;
    else if (bits == 16)
        *(uint16_t *)location = (uint16_t)value;
    else if (bits == 32)
        *(uint32_t *)location = (uint32_t)value;
    else
        *(uint64_t *)location = (uint64_t)value;
    return 0;
}

int module_elf_apply_relocation(uint32_t type, void *location,
                                uint64_t symbol_value, int64_t addend,
                                uintptr_t place)
{
    if (!location)
        return -E_INVAL;
    /* Symbol addresses live in the upper half of the canonical range, so
     * reinterpret them as signed 64-bit: an absolute reference to such an
     * address must be stored as a sign-extended 32-bit word. */
    __int128 absolute =
        (__int128)(int64_t)symbol_value + (__int128)addend;
    __int128 relative =
        absolute - (__int128)(int64_t)place;

    switch (type) {
    case R_X86_64_NONE:
        return 0;
    case R_X86_64_64:
    case R_X86_64_SIZE64:
        *(uint64_t *)location = symbol_value + (uint64_t)addend;
        return 0;
    case R_X86_64_PC32:
    case R_X86_64_PLT32:
        return store_signed(location, relative, 32);
    case R_X86_64_32:
    case R_X86_64_32S:
        return store_signed(location, absolute, 32);
    case R_X86_64_16:
        return store_unsigned(location, absolute, 16);
    case R_X86_64_PC16:
        return store_signed(location, relative, 16);
    case R_X86_64_8:
        return store_unsigned(location, absolute, 8);
    case R_X86_64_PC8:
        return store_signed(location, relative, 8);
    case R_X86_64_PC64:
        return store_signed(location, relative, 64);
    case R_X86_64_SIZE32:
        return store_unsigned(location, absolute, 32);
    default:
        return -E_NOEXEC;
    }
}