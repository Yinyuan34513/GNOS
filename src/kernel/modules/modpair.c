/*
 * modpair.c -- second demo module: imports moddemo_ticks() from moddemo.ko.
 * (GPLv2)
 *
 * This is the inter-module half of the loader: modpair has an undefined
 * symbol resolved against *another loaded module's* exports (not the
 * kernel's), and the import pins moddemo's refcount so delete_module on
 * moddemo alone fails with EBUSY until modpair is unloaded first.
 */
#include "module.h"
#include "module_elf.h"
#include "module_info.h"
#include "kstring.h"
#include "debugcon.h"

MODULE_NAME("modpair");
MODULE_LICENSE("GPL");

/* Defined by moddemo.ko; resolved by the loader against its exports. */
extern uint64_t moddemo_ticks(void);

static uint64_t seen;

int init_module(void)
{
    seen = moddemo_ticks();            /* import + pin moddemo */
    dbg_puts("MODPAIR: moddemo_ticks() = ");
    dbg_puts_dec((uint32_t)seen);
    dbg_puts("\r\n");
    return 0;
}

void cleanup_module(void)
{
    dbg_puts("MODPAIR: unloaded (imported ");
    dbg_puts_dec((uint32_t)seen);
    dbg_puts(")\r\n");
}