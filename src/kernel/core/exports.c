/*
 * exports.c -- symbols the kernel makes available to loadable modules.
 * (GPLv2)
 *
 * Every entry lands in the "__ksymtab" section via EXPORT_SYMBOL (see
 * module.h), and linker.ld brackets the section with __start___ksymtab /
 * __stop___ksymtab.  The module loader resolves a module's undefined
 * symbols against this table first, then against the exports of loaded
 * modules.
 *
 * The list is deliberately small: it is the *stable* API of the kernel.
 * A module may call anything here and nothing else.
 */
#include "module.h"
#include "kstring.h"
#include "heap.h"
#include "debugcon.h"
#include "timer.h"
#include "subsys.h"

EXPORT_SYMBOL(memcpy);
EXPORT_SYMBOL(memmove);
EXPORT_SYMBOL(memset);
EXPORT_SYMBOL(memcmp);
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(strcmp);
EXPORT_SYMBOL(strncmp);
EXPORT_SYMBOL(strncpy);

EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kfree);

EXPORT_SYMBOL(dbg_puts);
EXPORT_SYMBOL(dbg_puts_dec);
EXPORT_SYMBOL(dbg_puts_hex);

EXPORT_SYMBOL(timer_ticks);

/* The driver registry: a module can announce itself and be listed in
 * /proc/subsystems exactly like a built-in driver. */
EXPORT_SYMBOL(subsys_register);
EXPORT_SYMBOL(subsys_set_state);
EXPORT_SYMBOL(subsys_find);

/* Module pinning, for modules that export symbols to other modules. */
EXPORT_SYMBOL(try_module_get);
EXPORT_SYMBOL(module_put);
EXPORT_SYMBOL(module_refcount);