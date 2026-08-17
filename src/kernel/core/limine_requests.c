/*
 * limine_requests.c — Limine boot-protocol requests for GnOSKr.elf. (GPLv2)
 *
 * Limine scans the kernel image for the request markers, fills in the
 * responses before calling our entry point, and (via the entry-point
 * request) jumps straight to kernel_entry() in long mode.  The framebuffer
 * and the initrd module are delivered through framebuffer_request /
 * module_request below.
 */
#include <limine.h>

void kernel_entry(void);

__attribute__((used, section(".limine_requests_start"))) LIMINE_REQUESTS_START_MARKER

__attribute__((used, section(".limine_requests"))) LIMINE_BASE_REVISION(0)

__attribute__((used, section(".limine_requests"))) volatile struct limine_framebuffer_request framebuffer_request = {
    .id       = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests"))) volatile struct limine_module_request module_request = {
    .id       = LIMINE_MODULE_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests"))) volatile struct limine_hhdm_request hhdm_request = {
    .id       = LIMINE_HHDM_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests"))) volatile struct limine_memmap_request memmap_request = {
    .id       = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
};

/* Where the firmware left the ACPI root pointer.  Limine already had to find
 * it in order to boot, so asking is far more reliable than scanning the BIOS
 * area -- on a UEFI machine the RSDP is not down there at all.  acpi.c keeps
 * the scan only as a fallback. */
__attribute__((used, section(".limine_requests"))) volatile struct limine_rsdp_request rsdp_request = {
    .id       = LIMINE_RSDP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests"))) volatile struct limine_kernel_address_request kernel_address_request = {
    .id       = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests"))) volatile struct limine_entry_point_request entry_point_request = {
    .id       = LIMINE_ENTRY_POINT_REQUEST,
    .revision = 3,
    .entry    = &kernel_entry,
};

/* Ask Limine to discover the APs and hand us a per-CPU entry point so we can
 * bring them up.  Under the default (API revision 0) naming the type is
 * struct limine_smp_request; the id magic LIMINE_SMP_REQUEST is the same
 * 128-bit value regardless.  We fill in each AP's goto_address from
 * smp_init() once the response is populated. */
__attribute__((used, section(".limine_requests"))) volatile struct limine_smp_request smp_request = {
    .id       = LIMINE_SMP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests_end"))) LIMINE_REQUESTS_END_MARKER
