/*
 * module.h -- loadable kernel modules: registry, loader, lifecycle. (GPLv2)
 *
 * GNOS modules are plain ET_REL x86-64 objects, built like Linux .ko
 * files but without any of the modpost machinery: no vermagic, no
 * modversions, no signature.  A module's undefined symbols are resolved
 * against the kernel's exported symbol table (see exports.c) and against
 * the exports of already-loaded modules; its init/exit functions are
 * found by name (init_module / cleanup_module), like Uinxed-Kernel does.
 *
 * Everything a module may call is listed in exports.c; anything not
 * listed there is simply not resolvable.
 */
#ifndef GNUCOS_MODULE_H
#define GNUCOS_MODULE_H

#include <stdint.h>
#include <stddef.h>

#define MODULE_NAME_LEN 32
#define MODULE_MAX_SIZE (16U * 1024 * 1024)

/* module->state, mirroring Linux's enum module_state so /proc/modules
 * reads the same way. */
#define MODULE_STATE_UNFORMED 0
#define MODULE_STATE_COMING   1
#define MODULE_STATE_LIVE     2
#define MODULE_STATE_GOING    3

/* One entry of the kernel's exported-symbol table (section "__ksymtab").
 * The loader links the kernel's table and each module's own table into
 * one search space, so modules can both import kernel symbols and export
 * symbols to each other.
 *
 * Sized to 32 bytes on purpose: gcc places every variable of a section
 * at the section's alignment (16), so an entry whose natural size is 24
 * would end up with a 32-byte stride in the linked image while the
 * loader walks it with sizeof.  Padding to a multiple of 16 keeps the
 * stride equal to sizeof in every build. */
struct kernel_symbol {
    const char *name;       /* NUL-terminated, lives in rodata (kernel) or
                             * in the exporting module's mapping */
    uintptr_t   value;      /* runtime address of the symbol */
    uint8_t     gpl;        /* 1: only GPL-licensed modules may use it */
    uint8_t     reserved[15];
};

/* The public face of a loaded module.  Modules get a pointer to this
 * struct via the magic "__this_module" symbol, and pass it to
 * try_module_get()/module_put(). */
struct module {
    char     name[MODULE_NAME_LEN];
    int      state;
    uint32_t refcount;      /* try_module_get()/module_put() (plus
                             * imports of this module's exports) */
    size_t   core_size;     /* mapped bytes outside .init* */
    size_t   init_size;     /* mapped bytes in .init* sections */
};

/* Module-load flags for module_load() (init_module(2)). */
#define MODULE_INIT_IGNORE_MODVERSIONS 0x0001
#define MODULE_INIT_IGNORE_VERMAGIC    0x0002
#define MODULE_INIT_COMPRESSED_FILE    0x0004

/* Module-unload flags for module_unload() (delete_module(2)). */
#define MODULE_DELETE_NONBLOCK 0x0001
#define MODULE_DELETE_FORCE    0x0002

/* Export `sym` to loadable modules.  Only symbols with external linkage
 * can be exported (the macro needs `extern typeof(sym)` to compile). */
#define __EXPORT_SYMBOL(sym, gpl)                                       \
    extern typeof(sym) sym;                                             \
    static const struct kernel_symbol __ksym_##sym                      \
        __attribute__((used, section("__ksymtab"))) = {                 \
            #sym, (uintptr_t)&sym, (gpl) }

#define EXPORT_SYMBOL(sym)     __EXPORT_SYMBOL(sym, 0)
#define EXPORT_SYMBOL_GPL(sym) __EXPORT_SYMBOL(sym, 1)

/*
 * Load the ET_REL image at `image` (`size` bytes) as a kernel module.
 * `params` is accepted for Linux compat but GNOS modules have no
 * parameters (always NULL or "").  `hint` is a path used only to derive
 * the module name when the image carries no .modinfo "name=" field.
 * Returns 0 or a negative errno; on success the module is LIVE.
 */
int module_load(const void *image, size_t size, const char *params,
                unsigned int flags, const char *hint);

/* Unload a LIVE module by name.  Refused with -E_BUSY while its refcount
 * is nonzero unless MODULE_DELETE_FORCE is set. */
int module_unload(const char *name, unsigned int flags);

/* Render the /proc/modules table into `buffer`.  Returns bytes written. */
size_t module_format_proc(char *buffer, size_t size);

/* Pin / unpin a module so it cannot be unloaded while in use. */
int      try_module_get(struct module *module);
void     module_put(struct module *module);
uint32_t module_refcount(const struct module *module);

/* One-time registry setup; called from kernel_entry() after the heap. */
void module_subsys_init(void);

#endif /* GNUCOS_MODULE_H */