/*
 * kernel.c — GNOS 64-bit kernel (GNOSKr.elf), booted by Limine. (GPLv2)
 *
 * Limine enters kernel_entry() in long mode with paging already enabled:
 *   - the whole of physical RAM is direct-mapped at the HHDM offset
 *     (limine_hhdm_response.offset, normally 0xFFFF800000000000),
 *   - the kernel image itself lives at 0xFFFFFFFF80000000.
 * Every pointer Limine hands us (framebuffer address, module address, the
 * response structs themselves) is therefore an *already mapped* higher-half
 * virtual address -- we can dereference it straight away and must NOT try to
 * turn it back into a physical address.
 *
 * Bring-up order, and why:
 *   1. GDT/TSS and IDT first, so that from here on a bad memory access is a
 *      readable panic instead of a silent triple fault.
 *   2. Console, so the panic has somewhere to appear.
 *   3. PMM and VMM, because every later step allocates memory.
 *   4. VFS over the initrd, then the tty on top of it.
 *   5. The syscall gate and the timer.
 *   6. PID 1, built from /init.elf, and then the scheduler -- which never
 *      returns: this function becomes the idle loop.
 */
#include <stddef.h>
#include <stdint.h>

#include <limine.h>

#include "bootinfo.h"
#include "fbcon.h"
#include "debugcon.h"
#include "vfs.h"
#include "panic.h"
#include "gdt.h"
#include "idt.h"
#include "tty.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "gfx.h"
#include "fbdev.h"
#include "subsys.h"
#include "acpi.h"
#include "proc.h"
#include "timer.h"
#include "syscall.h"
#include "pci.h"
#include "e1000.h"
#include "net.h"
#include "ac97.h"
#include "hda.h"

extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile struct limine_module_request      module_request;
extern volatile struct limine_hhdm_request        hhdm_request;
extern volatile struct limine_kernel_address_request kernel_address_request;
extern volatile struct limine_rsdp_request        rsdp_request;

/* How often the scheduler pre-empts a running user process: SCHED_HZ, which
 * timer.h owns because times(2) and AT_CLKTCK have to report the same rate. */

/* bootinfo describing the framebuffer, shared with the console driver */
static bootinfo_t g_bi;

/* Physical/virtual geometry of the running kernel, filled in at entry. */
uint64_t g_hhdm;
uint64_t g_kernel_phys;
uint64_t g_kernel_virt;

void kernel_entry(void)
{
    dbg_puts("GNOS: kernel_entry reached\r\n");

    /* ---- higher-half direct map offset ------------------------------- */
    g_hhdm = hhdm_request.response ? hhdm_request.response->offset : 0;
    dbg_puts("GNOS: hhdm   = ");
    dbg_puts_hex(g_hhdm);
    dbg_puts("\r\n");

    /* ---- where Limine actually put us -------------------------------- */
    if (kernel_address_request.response) {
        g_kernel_phys = kernel_address_request.response->physical_base;
        g_kernel_virt = kernel_address_request.response->virtual_base;
    }
    dbg_puts("GNOS: kphys  = ");
    dbg_puts_hex(g_kernel_phys);
    dbg_puts("\r\nGNOS: kvirt  = ");
    dbg_puts_hex(g_kernel_virt);
    dbg_puts("\r\n");

    /* ---- framebuffer from Limine (HHDM virtual, already mapped) ------ */
    if (framebuffer_request.response &&
        framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb =
            framebuffer_request.response->framebuffers[0];
        g_bi.fb_addr   = (uint64_t)(uintptr_t)fb->address;
        g_bi.fb_width  = (uint32_t)fb->width;
        g_bi.fb_height = (uint32_t)fb->height;
        g_bi.fb_pitch  = (uint32_t)fb->pitch;
        g_bi.fb_bpp    = fb->bpp;
        g_bi.fb_type   = GNUCOS_FB_RGB;
    }

    /* ---- initrd module from Limine (HHDM virtual too) ---------------- */
    if (!module_request.response || module_request.response->module_count == 0)
        panic("no initrd module supplied by the bootloader");

    struct limine_file *mod = module_request.response->modules[0];
    g_bi.initrd_addr = (uint64_t)(uintptr_t)mod->address;
    g_bi.initrd_size = mod->size;
    dbg_puts("GNOS: initrd = ");
    dbg_puts_hex(g_bi.initrd_addr);
    dbg_puts(" size=");
    dbg_puts_dec((uint32_t)g_bi.initrd_size);
    dbg_puts("\r\n");

    /* ---- CPU tables --------------------------------------------------- */
    gdt_init();
    idt_init();

    /* The driver registry has to exist before the first driver init runs,
     * because every one of them announces itself into it. */
    subsys_init();

    /* ---- console ------------------------------------------------------ */
    fbcon_init(&g_bi);
    fbcon_puts("GNOS (x86-64) booted via Limine\n");

    /* ---- memory ------------------------------------------------------- */
    pmm_init();
    vmm_init();
    vmm_map_kernel_bss();              /* bootloader may leave BSS unmapped */
    kheap_init();                      /* kernel heap on top of the PMM */

    /* ---- firmware description ------------------------------------------
     * After the direct map is trustworthy (every ACPI pointer is physical and
     * gets read through it) and before the PCI probe, so that anything the
     * tables say about interrupt routing is already on hand. */
    acpi_init(rsdp_request.response
                  ? (uint64_t)(uintptr_t)rsdp_request.response->address : 0);
    acpi_dump();

    /* ---- PCI devices --------------------------------------------------
     * After the allocators, because every driver here needs DMA buffers and
     * an uncacheable MMIO mapping, and before the filesystem, so that a card
     * that wedges the machine does so with the boot log still short enough
     * to read.  Each driver announces itself and then proves itself: the
     * NIC loops a frame through its own PHY, the codec streams a tone past
     * the DMA engine.  Neither test needs a human, a network or a speaker. */
    pci_init();
    /* Each probe reports into the registry so that "was there a NIC?" is a
     * lookup later on instead of a line of boot text nobody kept. */
    int slot_nic = subsys_register("e1000", NULL, SUBSYS_CLASS_NET, 0, 0);
    if (e1000_init()) {
        subsys_set_state(slot_nic, SUBSYS_STATE_LIVE);
        e1000_selftest();
    } else {
        subsys_set_state(slot_nic, SUBSYS_STATE_FAILED);
    }

    int slot_ac97 = subsys_register("ac97", NULL, SUBSYS_CLASS_SOUND, 14, 3);
    if (ac97_init()) {
        subsys_set_state(slot_ac97, SUBSYS_STATE_LIVE);
        ac97_selftest();
    } else {
        subsys_set_state(slot_ac97, SUBSYS_STATE_FAILED);
    }

    int slot_hda = subsys_register("hda", NULL, SUBSYS_CLASS_SOUND, 116, 0);
    if (hda_init()) {
        subsys_set_state(slot_hda, SUBSYS_STATE_LIVE);
        hda_selftest();
    } else {
        subsys_set_state(slot_hda, SUBSYS_STATE_FAILED);
    }

    /* The IP stack sits on top of whatever the NIC probe found, so it is
     * configured here and not in e1000_init(): with no card it still comes up
     * with a working loopback, which is all `ping 127.0.0.1` needs. */
    net_init();

    /* ---- filesystem and drivers --------------------------------------- */
    fbcon_puts("initrd: mounting FAT filesystem...\n");
    if (!vfs_init((uint8_t *)(uintptr_t)g_bi.initrd_addr,
                  (uint32_t)g_bi.initrd_size))
        panic("initrd is not a mountable FAT volume");

    tty_init();

    /* /dev/fb0 goes in after the VFS (it needs the /dev table) but shares the
     * framebuffer with fbcon: the boot log stays on screen until a user-space
     * program opens the device and draws over it. */
    fbdev_init(&g_bi);

    syscall_init();

    /* ---- processes ----------------------------------------------------- */
    proc_init();
    timer_init(SCHED_HZ);

    kheap_self_test();
    gfx_self_test();
    fbdev_self_test();
    acpi_self_test();
    subsys_dump();

    fbcon_puts("starting /init.elf as pid 1...\n\n");
    int pid = proc_spawn_init("/init.elf");
    if (pid < 0)
        panic("cannot start /init.elf");

    /* init owns the terminal until it hands it to a child of its own. */
    tty_set_pgrp(pid);

    /* Never returns: this thread becomes the idle loop. */
    sched_start();
}
