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
#include "smp.h"
#include "tty.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "gfx.h"
#include "fbdev.h"
#include "input.h"
#include "subsys.h"
#include "module.h"
#include "acpi.h"
#include "lapic.h"
#include "proc.h"
#include "timer.h"
#include "syscall.h"
#include "pci.h"
#include "e1000.h"
#include "net.h"
#include "ac97.h"
#include "hda.h"
#include "ata.h"
#include "drm.h"
#include "procfs.h"

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
    /* Two ways to hold a root filesystem.  Live boot: a RAM module, which is
     * what the ISO ships.  Installed boot: nothing in RAM -- the root lives
     * in the first partition of the first ATA disk, written there by the
     * installer in user space.  The module wins when both exist.  The
     * disk-root branch is resolved later, once the allocator can give us
     * the 48 MiB a whole filesystem costs. */
    uint8_t *root_img   = NULL;
    uint32_t root_size  = 0;
    if (module_request.response && module_request.response->module_count > 0) {
        struct limine_file *mod = module_request.response->modules[0];
        g_bi.initrd_addr = (uint64_t)(uintptr_t)mod->address;
        g_bi.initrd_size = mod->size;
        root_img  = (uint8_t *)(uintptr_t)mod->address;
        root_size = (uint32_t)mod->size;
        dbg_puts("GNOS: initrd = ");
        dbg_puts_hex(g_bi.initrd_addr);
        dbg_puts(" size=");
        dbg_puts_dec((uint32_t)g_bi.initrd_size);
        dbg_puts("\r\n");
    } else {
        dbg_puts("GNOS: no initrd module; disk-root boot\r\n");
    }

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
    module_subsys_init();              /* loadable-module registry */

    /* ---- firmware description ------------------------------------------
     * After the direct map is trustworthy (every ACPI pointer is physical and
     * gets read through it) and before the PCI probe, so that anything the
     * tables say about interrupt routing is already on hand. */
    acpi_init(rsdp_request.response
                  ? (uint64_t)(uintptr_t)rsdp_request.response->address : 0);
    acpi_dump();
    acpi_pm1_init();

    /* ---- SMP and the local APIC -----------------------------------------
     * After ACPI (the MADT is where each LAPIC's MMIO base and the APs'
     * lapic ids come from) and before the filesystem, so a wedged AP shows
     * up early.  The APs reach ap_main() and idle in the scheduler; the
     * BSP enables its own LAPIC here (the PIT stays its clock). */
    smp_init();
    lapic_init();

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
    /* Resolve the root the allocator had no hand in earlier: a disk install
     * arrives with no RAM module, and its root is partition 1 of the first
     * ATA disk.  The driver names the disk /dev/sda while probing; the read
     * below bypasses the node entirely and just copies the partition into a
     * fresh 48 MiB arena, which is the only way a mounted filesystem can
     * stay put under a kernel that keeps the whole image resident. */
    if (!root_img) {
        fbcon_puts("no initrd module: reading root from disk 0 partition 1\n");
        ata_init();

        uint64_t frames = 16896;       /* 66 MiB: the image is 64 MB plus
                                        * margin for a grown one later */
        uint64_t phys   = pmm_alloc_contiguous(frames);
        if (!phys)
            panic("disk root: no memory for the root filesystem");
        root_img  = (uint8_t *)pmm_virt(phys);
        root_size = (uint32_t)ata_read_boot_partition(root_img,
                                                      (uint32_t)(frames * 4096));
        if (!root_size)
            panic("disk root: no root partition found on disk 0");
        g_bi.initrd_addr = phys;
        g_bi.initrd_size = root_size;
        dbg_puts("GNOS: disk root = ");
        dbg_puts_dec(root_size);
        dbg_puts(" bytes\r\n");
    }

    fbcon_puts("initrd: mounting EXT2 filesystem...\n");
    if (!vfs_init(root_img, root_size))
        panic("initrd is not a mountable ext2 volume");

    tty_init();

    /* The evdev driver publishes /dev/input/event0 (keyboard) and event1
     * (mouse) over the same i8042 ports tty_init just enabled.  It must
     * come after the VFS for the same /dev-table reason as fbdev. */
    input_init();

    /* /dev/fb0 goes in after the VFS (it needs the /dev table) but shares the
     * framebuffer with fbcon: the boot log stays on screen until a user-space
     * program opens the device and draws over it. */
    fbdev_init(&g_bi);

    /* The DRM/KMS driver publishes /dev/dri/card0 and /dev/dri/renderD128 on
     * top of the same framebuffer: dumb-buffer clients modeset through the
     * bochs VBE registers and blit their buffers to the screen. */
    drm_init(&g_bi);

    /* Disks come after the VFS for the same reason /dev/fb0 does -- they are
     * published as /dev nodes and there is no /dev before the mount.  Note
     * the ordering with respect to the *root* filesystem is the other way
     * round from a normal kernel: GNOS boots off an initrd that is already in
     * RAM, so ATA is not on the path to a running system.  It is here for the
     * installer to write to, and a machine with no disk at all boots exactly
     * as far as one with four.  ata_init() registers its own subsys slot,
     * because it discovers how many entries it needs while probing. */
    ata_init();

    /* ----. The rest of the root fs setup ---------------------------------
     * At this point the root filesystem is mounted and the disks are up, the
     * two halves a booted-from-disk system is built from.  The live-CD half
     * of the setup (RAM initrd) is out of date only in one respect: it wants
     * the *user space* half of the install machinery to run, and that half
     * needs to keep reading the boot payload from somewhere stable.  What it
     * reads (the kernel, the Limine stage files, the root image itself) was
     * published by this boot under /proc/boot/, which is as far from the
     * disk as it is possible to be: the blobs are boxes that hold the very
     * files an installer dumps onto a drive. */
    procfs_add_blob("/proc/boot/root.img", root_img, root_size);

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
