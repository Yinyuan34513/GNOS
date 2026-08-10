/*
 * pmm.h — physical page-frame allocator. (GPLv2)
 *
 * Hands out 4 KiB frames from the regions Limine marked USABLE.  Frames are
 * tracked in a bitmap that itself lives inside one of those regions, so the
 * allocator needs no memory of its own before it is running.
 *
 * Everything here deals in *physical* addresses; use pmm_virt() to get a
 * kernel-usable pointer (the HHDM direct map).
 */
#ifndef GNUCOS_PMM_H
#define GNUCOS_PMM_H

#include <stdint.h>

#define PAGE_SIZE 0x1000ULL

/* Build the bitmap from the Limine memory map.  Panics if there is no RAM. */
void pmm_init(void);

/* Allocate one frame.  Returns the physical address, or 0 when out of RAM. */
uint64_t pmm_alloc(void);

/* Allocate one frame and zero it.  Returns the physical address, or 0. */
uint64_t pmm_alloc_zeroed(void);

/* Release a frame previously returned by pmm_alloc(). */
void pmm_free(uint64_t phys);

/* Map a physical address into the kernel's direct map. */
void *pmm_virt(uint64_t phys);

/* Statistics, in frames. */
uint64_t pmm_total_frames(void);
uint64_t pmm_free_frames(void);

#endif
