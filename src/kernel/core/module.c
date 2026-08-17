/*
 * module.c -- loadable kernel module loader and registry. (GPLv2)
 *
 * Adapted from Uinxed-Kernel's kernel/module/module.c, trimmed hard to
 * what GNOS actually has underneath:
 *   - no vermagic / modversions / module signatures (our .ko files are
 *     built by our own Makefile; there is nobody to be incompatible with)
 *   - no module parameters (init_module's param argument is accepted for
 *     syscall compatibility and ignored)
 *   - no sysfs kobjects (the kernel has no sysfs; /proc/modules and
 *     /proc/subsystems are the introspection surface instead)
 *   - no per-section page protections: module pages are mapped RW like
 *     the kernel heap itself, which is the policy the whole kernel uses
 *
 * What remains is the real machinery: ELF relocation of an ET_REL image,
 * symbol resolution against the kernel's exported-symbol table and the
 * exports of already-loaded modules (with GPL marking and refcounted
 * imports), and the init/cleanup lifecycle.
 */
#include "module.h"
#include "module_elf.h"
#include "kstring.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "vfs.h"
#include "debugcon.h"

/* The kernel's own exported-symbol table, laid out by linker.ld. */
extern const struct kernel_symbol __start___ksymtab[];
extern const struct kernel_symbol __stop___ksymtab[];

/* The kernel's runtime base, from kernel.c (Limine's kernel_address
 * response).  Kept only for diagnostics. */
extern uint64_t g_kernel_virt;

/* Module address window: the classic Linux spot, the top 1 GiB of the
 * canonical range.  Modules are compiled -fno-pic, so they carry
 * R_X86_64_32 absolute relocations; a window in 0xFFFFFFFFC0000000..0xFFFFFFFF
 * lets every such value survive as a sign-extended 32-bit pointer. */
#define MODULE_VA_BASE   0xFFFFFFFFC0000000ULL
#define MODULE_VA_WINDOW 0x08000000ULL    /* 128 MiB of search space  */

typedef struct module_internal {
    struct module          *module;
    struct module_internal *next;
    void                    *image;        /* kernel copy of the file    */
    size_t                  size;
    module_elf_view_t       view;         /* validated view into image  */
    uintptr_t               base;         /* first mapped VA            */
    size_t                  mapped_size;  /* VA span, page multiple     */
    uintptr_t              *section_addr; /* per-section VA, 0 if none  */
    const struct kernel_symbol *exports;  /* this module's exports      */
    size_t                  export_count;
    const char             *license;      /* into image, never freed    */
    int                   (*init)(void);
    void                  (*exit)(void);
} module_internal_t;

static module_internal_t *module_list;
static volatile uint32_t  module_operation;   /* one load/unload at a time */

/* ---- small helpers ------------------------------------------------------ */

static int string_bounded(const char *string, size_t available)
{
    if (!string)
        return 0;
    for (size_t index = 0; index < available; index++)
        if (!string[index])
            return 1;
    return 0;
}

static const char *section_name(const module_elf_view_t *view, size_t index)
{
    if (!view || index >= view->section_count ||
        view->section_name_index == ELF64_SHN_UNDEF)
        return NULL;
    const elf64_shdr_t *strings = &view->sections[view->section_name_index];
    size_t offset = view->sections[index].sh_name;
    if (offset >= strings->sh_size)
        return NULL;
    const char *name =
        (const char *)view->image + strings->sh_offset + offset;
    return string_bounded(name, strings->sh_size - offset) ? name : NULL;
}

static const elf64_shdr_t *find_section(const module_elf_view_t *view,
                                        const char *wanted)
{
    for (size_t index = 0; index < view->section_count; index++) {
        const char *name = section_name(view, index);
        if (name && !strcmp(name, wanted))
            return &view->sections[index];
    }
    return NULL;
}

/* Value of the "key=" field in the .modinfo section, if present. */
static const char *modinfo_find(const module_elf_view_t *view,
                                const char *key, size_t *length_out)
{
    const elf64_shdr_t *section = find_section(view, ".modinfo");
    if (!section || !section->sh_size)
        return NULL;
    const char *data = (const char *)view->image + section->sh_offset;
    size_t key_size = strlen(key);
    size_t offset = 0;
    while (offset < section->sh_size) {
        size_t available = section->sh_size - offset;
        if (!string_bounded(data + offset, available))
            return NULL;
        size_t length = strlen(data + offset);
        if (length > key_size && data[offset + key_size] == '=' &&
            !strncmp(data + offset, key, key_size)) {
            if (length_out)
                *length_out = length - key_size - 1;
            return data + offset + key_size + 1;
        }
        offset += length + 1;
    }
    return NULL;
}

static int module_name_valid(const char *name)
{
    if (!name || !name[0])
        return 0;
    size_t length = 0;
    for (; name[length]; length++) {
        char c = name[length];
        if (length >= MODULE_NAME_LEN - 1 ||
            !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return length != 0;
}

static int license_gpl_compatible(const char *license)
{
    static const char *compatible[] = {
        "GPL", "GPL v2", "GPL and additional rights",
        "Dual BSD/GPL", "Dual MIT/GPL", "Dual MPL/GPL", NULL,
    };
    if (!license)
        return 0;
    for (size_t index = 0; compatible[index]; index++)
        if (!strcmp(license, compatible[index]))
            return 1;
    return 0;
}

static module_internal_t *module_find_locked(const char *name)
{
    for (module_internal_t *item = module_list; item; item = item->next)
        if (!strcmp(item->module->name, name))
            return item;
    return NULL;
}

static int operation_begin(void)
{
    if (module_operation)
        return -E_AGAIN;
    module_operation = 1;
    return 0;
}

static void operation_end(void)
{
    module_operation = 0;
}

/* ---- refcount (what keeps an unload honest) ----------------------------- */

uint32_t module_refcount(const struct module *module)
{
    return module ? module->refcount : 0;
}

int try_module_get(struct module *module)
{
    if (!module || module->state != MODULE_STATE_LIVE ||
        module->refcount == 0xffffffffu)
        return 0;
    module->refcount++;
    return 1;
}

void module_put(struct module *module)
{
    if (module && module->refcount)
        module->refcount--;
}

/* ---- symbol resolution -------------------------------------------------- */

static int symbol_usable(module_internal_t *consumer,
                         const struct kernel_symbol *symbol)
{
    if (!symbol || !symbol->name || !symbol->value)
        return 0;
    if (symbol->gpl && !license_gpl_compatible(consumer->license))
        return 0;
    return 1;
}

/* Find `name` among the kernel's exports, then among the exports of every
 * loaded module (which also pins that module so it cannot unload under us).
 * Returns 0 with *value_out set, or a negative errno. */
static int resolve_export(module_internal_t *consumer, const char *name,
                          uint64_t *value_out)
{
    for (const struct kernel_symbol *symbol = __start___ksymtab;
         symbol < __stop___ksymtab; symbol++) {
        if (symbol->name && !strcmp(symbol->name, name)) {
            if (!symbol_usable(consumer, symbol))
                return -E_PERM;
            *value_out = symbol->value;
            return 0;
        }
    }

    for (module_internal_t *item = module_list; item; item = item->next) {
        if (item == consumer || item->module->state != MODULE_STATE_LIVE)
            continue;
        for (size_t index = 0; index < item->export_count; index++) {
            const struct kernel_symbol *symbol = &item->exports[index];
            if (symbol->name && !strcmp(symbol->name, name)) {
                if (!symbol_usable(consumer, symbol))
                    return -E_PERM;
                if (!try_module_get(item->module))
                    return -E_BUSY;
                *value_out = symbol->value;
                return 0;
            }
        }
    }
    return -E_NOENT;
}

/* ---- section layout and mapping ----------------------------------------- */

static int align_up(size_t value, size_t alignment, size_t *result)
{
    if (value > (size_t)-1 - (alignment - 1))
        return -E_OVERFLOW;
    *result = (value + alignment - 1) & ~(alignment - 1);
    return 0;
}

/* Find a free run of `size` bytes in the module window, scanning the
 * kernel page table so we never collide with a fixmap or future use. */
static uintptr_t module_find_va(size_t size)
{
    uintptr_t start = MODULE_VA_BASE;
    uintptr_t limit = start + MODULE_VA_WINDOW;
    uintptr_t run = 0;
    size_t    got = 0;
    for (uintptr_t va = start; va < limit; va += PAGE_SIZE) {
        if (vmm_kernel_present(va)) {
            run = 0;
            got = 0;
            continue;
        }
        if (!run)
            run = va;
        got += PAGE_SIZE;
        if (got >= size)
            return run;
    }
    return 0;
}

static int layout_sections(module_internal_t *internal,
                           const module_elf_view_t *view)
{
    internal->section_addr =
        (uintptr_t *)kmalloc(view->section_count * sizeof(uintptr_t));
    if (!internal->section_addr)
        return -E_NOMEM;
    for (size_t i = 0; i < view->section_count; i++)
        internal->section_addr[i] = 0;

    size_t total = 0;
    for (size_t index = 0; index < view->section_count; index++) {
        const elf64_shdr_t *section = &view->sections[index];
        if (!(section->sh_flags & ELF64_SHF_ALLOC))
            continue;
        if (section->sh_flags & ELF64_SHF_EXECINSTR &&
            section->sh_flags & ELF64_SHF_WRITE)
            return -E_NOEXEC;             /* W+X module code: refused */
        size_t alignment = section->sh_addralign;
        if (alignment < PAGE_SIZE)
            alignment = PAGE_SIZE;
        int ret = align_up(total, alignment, &total);
        if (ret != 0)
            return ret;
        size_t mapped = 0;
        ret = align_up(section->sh_size, PAGE_SIZE, &mapped);
        if (ret != 0)
            return ret;
        if (total > MODULE_MAX_SIZE - mapped)
            return -E_2BIG;
        const char *name = section_name(view, index);
        if (!name)
            return -E_NOEXEC;
        if (!strncmp(name, ".init", 5))
            internal->module->init_size += mapped;
        else
            internal->module->core_size += mapped;
        total += mapped;
    }
    if (!total)
        return -E_NOEXEC;
    internal->mapped_size = total;
    return 0;
}

static int map_sections(module_internal_t *internal,
                        const module_elf_view_t *view)
{
    internal->base = module_find_va(internal->mapped_size);
    if (!internal->base)
        return -E_NOMEM;

    size_t offset = 0;
    size_t mapped_so_far = 0;
    for (size_t index = 0; index < view->section_count; index++) {
        const elf64_shdr_t *section = &view->sections[index];
        if (!(section->sh_flags & ELF64_SHF_ALLOC))
            continue;
        size_t alignment = section->sh_addralign;
        if (alignment < PAGE_SIZE)
            alignment = PAGE_SIZE;
        if (align_up(offset, alignment, &offset) != 0)
            goto fail;
        internal->section_addr[index] = internal->base + offset;
        size_t mapped = 0;
        if (align_up(section->sh_size, PAGE_SIZE, &mapped) != 0)
            goto fail;

        for (size_t page = 0; page < mapped / PAGE_SIZE; page++) {
            uint64_t frame = pmm_alloc();
            if (!frame)
                goto fail;
            if (!vmm_map_kernel(internal->base + offset + page * PAGE_SIZE,
                                frame, VM_WRITE | VM_EXEC)) {
                pmm_free(frame);
                goto fail;
            }
        }
        /* Zero first, then fill: NOBITS sections must read as zeros. */
        memset((void *)internal->section_addr[index], 0, section->sh_size);
        if (section->sh_type != ELF64_SHT_NOBITS && section->sh_size)
            memcpy((void *)internal->section_addr[index],
                   (const uint8_t *)view->image + section->sh_offset,
                   section->sh_size);
        offset += mapped;
        mapped_so_far = offset;
    }
    return 0;

fail:
    if (mapped_so_far)
        vmm_unmap_kernel(internal->base, mapped_so_far);
    return -E_NOMEM;
}

/* ---- relocation --------------------------------------------------------- */

static size_t relocation_width(uint32_t type)
{
    switch (type) {
    case 0:                  /* R_X86_64_NONE */
        return 0;
    case 14:                 /* R_X86_64_8   */
    case 15:                 /* R_X86_64_PC8  */
        return 1;
    case 12:                 /* R_X86_64_16  */
    case 13:                 /* R_X86_64_PC16 */
        return 2;
    case 2:                  /* R_X86_64_PC32   */
    case 4:                  /* R_X86_64_PLT32  */
    case 10:                 /* R_X86_64_32     */
    case 11:                 /* R_X86_64_32S    */
    case 32:                 /* R_X86_64_SIZE32 */
        return 4;
    case 1:                  /* R_X86_64_64    */
    case 24:                 /* R_X86_64_PC64  */
    case 33:                 /* R_X86_64_SIZE64 */
        return 8;
    default:
        return (size_t)-1;
    }
}

static int symbol_name_at(const module_elf_view_t *view,
                          const elf64_shdr_t *symbols, const elf64_sym_t *symbol,
                          const char **name_out)
{
    if (symbols->sh_link >= view->section_count)
        return -E_NOEXEC;
    const elf64_shdr_t *strings = &view->sections[symbols->sh_link];
    if (strings->sh_type != ELF64_SHT_STRTAB ||
        symbol->st_name >= strings->sh_size)
        return -E_NOEXEC;
    const char *name =
        (const char *)view->image + strings->sh_offset + symbol->st_name;
    if (!string_bounded(name, strings->sh_size - symbol->st_name))
        return -E_NOEXEC;
    *name_out = name;
    return 0;
}

static int resolve_elf_symbol(module_internal_t *internal,
                              const module_elf_view_t *view,
                              const elf64_shdr_t *symbols,
                              const elf64_sym_t *symbol, uint64_t *value_out)
{
    const char *name = NULL;
    int ret = symbol_name_at(view, symbols, symbol, &name);
    if (ret != 0)
        return ret;
    if (name[0] && !strcmp(name, "__this_module")) {
        *value_out = (uintptr_t)internal->module;
        return 0;
    }
    if (symbol->st_shndx == ELF64_SHN_ABS) {
        *value_out = symbol->st_value;
        return 0;
    }
    if (symbol->st_shndx == ELF64_SHN_UNDEF) {
        if (!name[0])
            return -E_NOEXEC;
        ret = resolve_export(internal, name, value_out);
        if (ret == -E_NOENT && (symbol->st_info >> 4) == ELF64_STB_WEAK) {
            *value_out = 0;
            return 0;
        }
        return ret;
    }
    if (symbol->st_shndx == ELF64_SHN_COMMON ||
        symbol->st_shndx == ELF64_SHN_XINDEX ||
        symbol->st_shndx >= view->section_count)
        return -E_NOEXEC;
    uintptr_t addr = internal->section_addr[symbol->st_shndx];
    if (!addr || symbol->st_value > view->sections[symbol->st_shndx].sh_size)
        return -E_NOEXEC;
    *value_out = addr + symbol->st_value;
    return 0;
}

static int relocate_module(module_internal_t *internal,
                           const module_elf_view_t *view)
{
    for (size_t section_index = 0; section_index < view->section_count;
         section_index++) {
        const elf64_shdr_t *relocations = &view->sections[section_index];
        if (relocations->sh_type != ELF64_SHT_RELA)
            continue;
        if (relocations->sh_info >= view->section_count ||
            relocations->sh_link >= view->section_count)
            return -E_NOEXEC;
        uintptr_t target = internal->section_addr[relocations->sh_info];
        if (!target)
            continue;
        const elf64_shdr_t *symbols = &view->sections[relocations->sh_link];
        if (symbols->sh_type != ELF64_SHT_SYMTAB ||
            symbols->sh_entsize != sizeof(elf64_sym_t))
            return -E_NOEXEC;
        const elf64_sym_t *symbol_table =
            (const elf64_sym_t *)((const uint8_t *)view->image + symbols->sh_offset);
        size_t symbol_count = symbols->sh_size / sizeof(elf64_sym_t);
        const elf64_rela_t *table =
            (const elf64_rela_t *)((const uint8_t *)view->image + relocations->sh_offset);
        size_t count = relocations->sh_size / sizeof(elf64_rela_t);

        for (size_t index = 0; index < count; index++) {
            uint32_t type = ELF64_R_TYPE(table[index].r_info);
            size_t width = relocation_width(type);
            size_t symbol_index = ELF64_R_SYM(table[index].r_info);
            size_t target_size = view->sections[relocations->sh_info].sh_size;
            if (width == (size_t)-1 || symbol_index >= symbol_count ||
                table[index].r_offset > target_size ||
                width > target_size - table[index].r_offset) {
                dbg_puts("MODULE: reloc bad type=");
                dbg_puts_dec(type);
                dbg_puts(" sym=");
                dbg_puts_dec(symbol_index);
                dbg_puts("/");
                dbg_puts_dec(symbol_count);
                dbg_puts(" off=");
                dbg_puts_dec(table[index].r_offset);
                dbg_puts("/");
                dbg_puts_dec(target_size);
                dbg_puts(" width=");
                dbg_puts_dec(width);
                dbg_puts("\r\n");
                return -E_NOEXEC;
            }
            const elf64_sym_t *symbol = &symbol_table[symbol_index];
            if ((symbol->st_info & ELF64_ST_TYPE_M) == 10 ||      /* GNU_IFUNC */
                (symbol->st_info & ELF64_ST_TYPE_M) == 6) {        /* TLS      */
                dbg_puts("MODULE: reloc ifunc/tls type=");
                dbg_puts_dec(type);
                dbg_puts(" sym=");
                dbg_puts_dec(symbol_index);
                dbg_puts("\r\n");
                return -E_NOEXEC;
            }
            uint64_t value = 0;
            int ret = resolve_elf_symbol(internal, view, symbols, symbol,
                                         &value);
            if (ret != 0) {
                dbg_puts("MODULE: reloc fail sec=");
                dbg_puts_dec(section_index);
                dbg_puts(" type=");
                dbg_puts_dec(type);
                dbg_puts(" sym=");
                dbg_puts_dec(symbol_index);
                dbg_puts(" shndx=");
                dbg_puts_dec(symbol->st_shndx);
                dbg_puts(" ret=");
                dbg_puts_dec(-ret);
                dbg_puts("\r\n");
                return ret;
            }
            if (type == 32 || type == 33)        /* SIZE32 / SIZE64 */
                value = symbol->st_size;
            uintptr_t place = target + table[index].r_offset;
            ret = module_elf_apply_relocation(type, (void *)place, value,
                                              table[index].r_addend, place);
            if (ret != 0) {
                dbg_puts("MODULE: reloc apply fail type=");
                dbg_puts_dec(type);
                dbg_puts(" sym=");
                dbg_puts_dec(symbol_index);
                dbg_puts(" ret=");
                dbg_puts_dec(-ret);
                dbg_puts("\r\n");
                return ret;
            }
        }
    }
    return 0;
}

/* ---- lifecycle discovery and exports ------------------------------------ */

static int module_range_mapped(module_internal_t *internal, uintptr_t address,
                               size_t size)
{
    if (!internal || address < internal->base ||
        size > internal->mapped_size)
        return 0;
    return address - internal->base <= internal->mapped_size - size;
}

static int module_string_valid(module_internal_t *internal, const char *string)
{
    uintptr_t address = (uintptr_t)string;
    if (!module_range_mapped(internal, address, 1))
        return 0;
    size_t limit = internal->mapped_size - (address - internal->base);
    for (size_t index = 0; index < limit; index++) {
        if (!string[index])
            return 1;
    }
    return 0;
}

static int find_lifecycle(module_internal_t *internal,
                          const module_elf_view_t *view)
{
    const elf64_shdr_t *symbols = NULL;
    for (size_t index = 0; index < view->section_count; index++) {
        if (view->sections[index].sh_type == ELF64_SHT_SYMTAB) {
            symbols = &view->sections[index];
            break;
        }
    }
    if (!symbols)
        return -E_NOEXEC;
    const elf64_sym_t *table =
        (const elf64_sym_t *)((const uint8_t *)view->image + symbols->sh_offset);
    size_t count = symbols->sh_size / sizeof(elf64_sym_t);
    for (size_t index = 0; index < count; index++) {
        const char *name = NULL;
        if (symbol_name_at(view, symbols, &table[index], &name) != 0)
            return -E_NOEXEC;
        if (!name[0] || (strcmp(name, "init_module") &&
                         strcmp(name, "cleanup_module")))
            continue;
        uint64_t value = 0;
        int ret = resolve_elf_symbol(internal, view, symbols, &table[index],
                                     &value);
        if (ret != 0 || !module_range_mapped(internal, value, 1))
            return -E_NOEXEC;
        if (!strcmp(name, "init_module"))
            internal->init = (int (*)(void))value;
        else
            internal->exit = (void (*)(void))value;
    }
    return 0;
}

/* The module's own __ksymtab, relocated by now; verify every entry points
 * back into the module and that no name collides with the kernel's table
 * or another loaded module's. */
static int validate_exports(module_internal_t *internal,
                            const module_elf_view_t *view)
{
    const elf64_shdr_t *section = find_section(view, "__ksymtab");
    if (!section)
        return 0;
    if (!internal->section_addr[section - view->sections] ||
        section->sh_size % sizeof(struct kernel_symbol))
        return -E_NOEXEC;
    internal->exports =
        (const struct kernel_symbol *)internal->section_addr[section - view->sections];
    internal->export_count = section->sh_size / sizeof(struct kernel_symbol);

    for (size_t index = 0; index < internal->export_count; index++) {
        const struct kernel_symbol *symbol = &internal->exports[index];
        if (!module_string_valid(internal, symbol->name) ||
            !symbol->name[0] ||
            !module_range_mapped(internal, symbol->value, 1))
            return -E_NOEXEC;
        for (size_t prior = 0; prior < index; prior++)
            if (!strcmp(internal->exports[prior].name, symbol->name))
                return -E_EXIST;
        for (const struct kernel_symbol *core = __start___ksymtab;
             core < __stop___ksymtab; core++)
            if (core->name && !strcmp(core->name, symbol->name))
                return -E_EXIST;
        for (module_internal_t *item = module_list; item; item = item->next)
            for (size_t existing = 0; existing < item->export_count;
                 existing++)
                if (!strcmp(item->exports[existing].name, symbol->name))
                    return -E_EXIST;
    }
    return 0;
}

/* ---- teardown ----------------------------------------------------------- */

static void destroy_internal(module_internal_t *internal)
{
    if (!internal)
        return;
    if (internal->mapped_size)
        vmm_unmap_kernel(internal->base, internal->mapped_size);
    kfree(internal->section_addr);
    kfree(internal->image);
    kfree(internal->module);
    kfree(internal);
}

static void remove_registry(module_internal_t *internal)
{
    module_internal_t **link = &module_list;
    while (*link) {
        if (*link == internal) {
            *link = internal->next;
            break;
        }
        link = &(*link)->next;
    }
    internal->next = NULL;
}

/* ---- load / unload ------------------------------------------------------ */

int module_load(const void *image, size_t size, const char *params,
                unsigned int flags, const char *hint)
{
    (void)params;                          /* GNOS modules have no params */
    if (!image || !size || size > MODULE_MAX_SIZE)
        return !image ? -E_FAULT : -E_2BIG;
    if (flags & ~(MODULE_INIT_IGNORE_MODVERSIONS | MODULE_INIT_IGNORE_VERMAGIC |
                  MODULE_INIT_COMPRESSED_FILE))
        return -E_INVAL;
    if (flags & MODULE_INIT_COMPRESSED_FILE)
        return -E_OPNOTSUPP;
    int result = operation_begin();
    if (result != 0)
        return result;

    /* The caller's image may be user memory; work from a kernel copy. */
    void *copy = kmalloc(size);
    if (!copy) {
        operation_end();
        return -E_NOMEM;
    }
    memcpy(copy, image, size);

    module_elf_view_t view;
    result = module_elf_validate(copy, size, &view);
    if (result != 0) {
        kfree(copy);
        operation_end();
        return result;
    }

    module_internal_t *internal = (module_internal_t *)kmalloc(sizeof(*internal));
    struct module *module = (struct module *)kmalloc(sizeof(*module));
    if (!internal || !module) {
        kfree(internal);
        kfree(module);
        kfree(copy);
        operation_end();
        return -E_NOMEM;
    }
    memset(internal, 0, sizeof(*internal));
    memset(module, 0, sizeof(*module));
    internal->module = module;
    internal->image  = copy;
    internal->size   = size;
    internal->view   = view;
    module->state = MODULE_STATE_UNFORMED;

    /* ---- TEMP stage tracing ---- */
    const char *stage = "validate";

    /* Name: .modinfo "name=" wins; else the file's basename without .ko. */
    size_t name_length = 0;
    const char *name = modinfo_find(&view, "name", &name_length);
    char owned_name[MODULE_NAME_LEN];
    if (!name) {
        stage = "hint";
        if (!hint) {
            result = -E_NOEXEC;
            goto out;
        }
        const char *base = hint;
        for (const char *p = hint; *p; p++)
            if (*p == '/')
                base = p + 1;
        name_length = strlen(base);
        if (name_length > 3 && !strcmp(base + name_length - 3, ".ko"))
            name_length -= 3;
        if (name_length >= MODULE_NAME_LEN) {
            result = -E_NOEXEC;
            goto out;
        }
        memcpy(owned_name, base, name_length);
        owned_name[name_length] = 0;
        name = owned_name;
    }
    stage = "name";
    if (name_length >= MODULE_NAME_LEN || !module_name_valid(name)) {
        result = -E_INVAL;
        goto out;
    }
    memcpy(module->name, name, name_length);
    module->name[name_length] = 0;

    /* License: .modinfo "license=", defaulting to GPL for our own
     * toolchain.  Anything not GPL-compatible is refused GPL-only
     * exports. */
    internal->license = modinfo_find(&view, "license", NULL);
    if (!internal->license)
        internal->license = "GPL";

    stage = "exist";
    if (module_find_locked(module->name)) {
        result = -E_EXIST;
        goto out;
    }

    stage = "layout";
    result = layout_sections(internal, &view);
    if (result != 0)
        goto out;
    stage = "map";
    result = map_sections(internal, &view);
    if (result != 0)
        goto out;
    stage = "relocate";
    result = relocate_module(internal, &view);
    if (result != 0)
        goto out;
    stage = "lifecycle";
    result = find_lifecycle(internal, &view);
    if (result != 0)
        goto out;
    stage = "exports";
    result = validate_exports(internal, &view);
    if (result != 0)
        goto out;
    /* Make the writes visible before the module code can run. */
    asm volatile("mfence" ::: "memory");

    module->state = MODULE_STATE_COMING;
    internal->next = module_list;
    module_list = internal;

    if (internal->init) {
        int init_result = internal->init();
        if (init_result != 0) {
            result = init_result < 0 ? init_result : -E_INVAL;
            module->state = MODULE_STATE_GOING;
            remove_registry(internal);
            goto out;
        }
    }
    module->state = MODULE_STATE_LIVE;
    operation_end();
    return 0;

out:
    if (result != 0) {
        dbg_puts("MODULE: load failed @ ");
        dbg_puts(stage);
        dbg_puts(" -> ");
        dbg_puts_dec(-result);
        dbg_puts("\r\n");
    }
    destroy_internal(internal);
    operation_end();
    return result;
}

int module_unload(const char *name, unsigned int flags)
{
    if (!module_name_valid(name) ||
        flags & ~(MODULE_DELETE_NONBLOCK | MODULE_DELETE_FORCE))
        return -E_INVAL;
    int result = operation_begin();
    if (result != 0)
        return flags & MODULE_DELETE_NONBLOCK ? -E_AGAIN : result;

    module_internal_t *internal = module_find_locked(name);
    if (!internal) {
        result = -E_NOENT;
        goto out;
    }
    if (internal->module->state != MODULE_STATE_LIVE) {
        result = -E_BUSY;
        goto out;
    }
    if (module_refcount(internal->module) && !(flags & MODULE_DELETE_FORCE)) {
        result = -E_BUSY;
        goto out;
    }
    if (module_refcount(internal->module))
        internal->module->refcount = 0;    /* FORCE */

    internal->module->state = MODULE_STATE_GOING;
    if (internal->exit)
        internal->exit();
    remove_registry(internal);
    destroy_internal(internal);
    result = 0;
out:
    operation_end();
    return result;
}

/* ---- /proc/modules ------------------------------------------------------ */

static size_t proc_append_dec(char *buffer, size_t size, size_t written,
                              uint64_t value)
{
    char digits[20];
    int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n && written < size - 1)
        buffer[written++] = digits[--n];
    return written;
}

size_t module_format_proc(char *buffer, size_t size)
{
    if (!buffer || !size)
        return 0;
    size_t written = 0;
    for (module_internal_t *item = module_list;
         item && written < size - 1; item = item->next) {
        /* Linux layout: name size refcount users state address */
        for (size_t i = 0; item->module->name[i] && written < size - 1; i++)
            buffer[written++] = item->module->name[i];
        buffer[written++] = ' ';
        written = proc_append_dec(buffer, size, written,
                                  item->module->core_size + item->module->init_size);
        buffer[written++] = ' ';
        written = proc_append_dec(buffer, size, written,
                                  module_refcount(item->module));
        buffer[written++] = ' ';
        buffer[written++] = '-';
        buffer[written++] = ' ';
        const char *state = item->module->state == MODULE_STATE_LIVE
                                ? "Live" : "coming";
        for (size_t i = 0; state[i] && written < size - 1; i++)
            buffer[written++] = state[i];
        buffer[written++] = ' ';
        /* Address in the Linux style: 0x%llx */
        buffer[written++] = '0';
        buffer[written++] = 'x';
        for (int shift = 56; shift >= 0 && written < size - 1; shift -= 4) {
            unsigned digit = (unsigned)(item->base >> shift) & 0xF;
            buffer[written++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
        }
        buffer[written++] = '\n';
    }
    buffer[written] = 0;
    return written;
}

void module_subsys_init(void)
{
    module_list = NULL;
    module_operation = 0;
    dbg_puts("MODULE: registry ready\r\n");
    dbg_puts("MODULE: ksymtab ");
    dbg_puts_hex((uintptr_t)__start___ksymtab);
    dbg_puts("..");
    dbg_puts_hex((uintptr_t)__stop___ksymtab);
    dbg_puts(" count=");
    dbg_puts_dec((uintptr_t)(__stop___ksymtab - __start___ksymtab));
    dbg_puts("\r\n");
    if (__start___ksymtab < __stop___ksymtab) {
        dbg_puts("MODULE: first export name=");
        dbg_puts(__start___ksymtab[0].name);
        dbg_puts(" value=");
        dbg_puts_hex(__start___ksymtab[0].value);
        dbg_puts(" gpl=");
        dbg_puts_dec(__start___ksymtab[0].gpl);
        dbg_puts("\r\n");
    }
}