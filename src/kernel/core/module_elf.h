/*
 * module_elf.h -- ELF-64 structures and views for loadable kernel modules.
 * (GPLv2)
 *
 * GNOS modules are plain ET_REL (relocatable) x86-64 ELF objects, like
 * Linux .ko files.  This header defines the minimal subset of the ELF-64
 * on-disk format the loader needs, plus the validated view of an image the
 * loader passes around.
 */
#ifndef GNUCOS_MODULE_ELF_H
#define GNUCOS_MODULE_ELF_H

#include <stdint.h>
#include <stddef.h>

/* ---- ELF-64 constants (only what ET_REL module loading needs) ---------- */
#define ELF64_ET_REL     1
#define ELF64_EM_X86_64 62

#define ELF64_SHT_NOBITS 8
#define ELF64_SHT_SYMTAB 2
#define ELF64_SHT_STRTAB 3
#define ELF64_SHT_RELA   4

#define ELF64_SHN_UNDEF      0
#define ELF64_SHN_ABS     0xfff1
#define ELF64_SHN_COMMON  0xfff2
#define ELF64_SHN_XINDEX  0xffff

#define ELF64_SHF_ALLOC   0x2
#define ELF64_SHF_WRITE   0x1
#define ELF64_SHF_EXECINSTR 0x4

#define ELF64_STB_WEAK    2
#define ELF64_ST_TYPE_M   0xf

#define ELF64_R_SYM(i)    ((i) >> 32)
#define ELF64_R_TYPE(i)   ((i) & 0xffffffffUL)

typedef struct {
    unsigned char e_ident[16];
    uint16_t      e_type;
    uint16_t      e_machine;
    uint32_t      e_version;
    uint64_t      e_entry;
    uint64_t      e_phoff;
    uint64_t      e_shoff;
    uint32_t      e_flags;
    uint16_t      e_ehsize;
    uint16_t      e_phentsize;
    uint16_t      e_phnum;
    uint16_t      e_shentsize;
    uint16_t      e_shnum;
    uint16_t      e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t      sh_name;
    uint32_t      sh_type;
    uint64_t      sh_flags;
    uint64_t      sh_addr;
    uint64_t      sh_offset;
    uint64_t      sh_size;
    uint32_t      sh_link;
    uint32_t      sh_info;
    uint64_t      sh_addralign;
    uint64_t      sh_entsize;
} elf64_shdr_t;

typedef struct {
    uint32_t      st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t      st_shndx;
    uint64_t      st_value;
    uint64_t      st_size;
} elf64_sym_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} elf64_rela_t;

/* A validated handle on a module image: every field the loader walks is
 * checked to lie inside the image, so the rest of the loader can index
 * these arrays without re-checking offsets. */
typedef struct {
    const void      *image;
    size_t           size;
    const elf64_ehdr_t *header;
    const elf64_shdr_t *sections;
    size_t           section_count;
    size_t           section_name_index;
} module_elf_view_t;

/* Validate `size` bytes at `image` as an ET_REL x86-64 object and fill in
 * `view`.  Returns 0 or a negative errno. */
int module_elf_validate(const void *image, size_t size, module_elf_view_t *view);

/* Apply one relocation of `type` at `location` (already mapped into kernel
 * memory): S + A for absolute types, S + A - P for PC-relative ones.
 * Returns 0 or a negative errno. */
int module_elf_apply_relocation(uint32_t type, void *location,
                                uint64_t symbol_value, int64_t addend,
                                uintptr_t place);

#endif /* GNUCOS_MODULE_ELF_H */