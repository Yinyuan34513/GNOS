/*
 * moddemo.c -- demo loadable kernel module for GNOS. (GPLv2)
 *
 * Does three real things, each of which can be observed from user space:
 *   1. publishes itself through the driver registry (subsys_register), so
 *      it shows up in /proc/subsystems as a live device class,
 *   2. exports a symbol (moddemo_ticks) that other modules can import --
 *      modpair.ko does, which also pins this module's refcount,
 *   3. prints on load and on unload, so the debug console proves the
 *      lifecycle ran.
 */
#include "module.h"
#include "module_elf.h"
#include "module_info.h"
#include "kstring.h"
#include "debugcon.h"
#include "subsys.h"
#include "vfs.h"
#include "timer.h"

MODULE_NAME("moddemo");
MODULE_LICENSE("GPL");

static uint64_t load_ticks;
static uint32_t calls;

/* Exported to other modules: how many times the tick counter has been
 * sampled since this module loaded. */
uint64_t moddemo_ticks(void)
{
    calls++;
    return timer_ticks() - load_ticks;
}

EXPORT_SYMBOL(moddemo_ticks);

/* The loader looks up init_module()/cleanup_module() by name. */
int init_module(void)
{
    load_ticks = timer_ticks();
    calls = 0;
    int slot = subsys_register("moddemo", NULL, SUBSYS_CLASS_OTHER, 0, 0);
    if (slot < 0)
        return -E_IO;
    subsys_set_state(slot, SUBSYS_STATE_LIVE);
    dbg_puts("MODDEMO: loaded (tick ");
    dbg_puts_dec((uint32_t)load_ticks);
    dbg_puts(")\r\n");
    return 0;
}

void cleanup_module(void)
{
    int slot = subsys_find("moddemo");
    if (slot >= 0)
        subsys_set_state(slot, SUBSYS_STATE_REGISTERED);
    dbg_puts("MODDEMO: unloaded (calls=");
    dbg_puts_dec(calls);
    dbg_puts(")\r\n");
}