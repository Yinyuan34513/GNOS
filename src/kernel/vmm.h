/*
 * vmm.h — per-process virtual address spaces. (GPLv2)
 *
 * Each process gets its own PML4.  The upper half (entries 256..511: the
 * kernel image and the HHDM direct map) is *shared* by copying Limine's
 * entries into every new table, so a trap taken in any process can still
 * reach kernel code and any physical page.  The lower half is private, which
 * is what actually isolates one user program from another.
 */
#ifndef GNUCOS_VMM_H
#define GNUCOS_VMM_H

#include <stdint.h>

#define VM_READ   0x0
#define VM_WRITE  0x1
#define VM_USER   0x2
#define VM_EXEC   0x4

/* Where a user process is laid out.
 *
 * The lower half is carved into three non-overlapping arenas:
 *   - brk heap:       USER_BRK_BASE .. USER_BRK_CEIL   (grows upward)
 *   - mmap arena:     USER_MMAP_BASE .. USER_MMAP_CEIL (grows upward)
 *   - stack:          USER_STACK_TOP - USER_STACK_SIZE .. USER_STACK_TOP
 * The brk heap and the mmap arena must NOT overlap: musl's malloc hands out
 * both brk-backed and mmap-backed chunks, and if their ranges interleave the
 * heap metadata gets corrupted and malloc aborts.  Keep a gap before the stack
 * too, or a growing heap collides with a growing stack. */
#define USER_STACK_TOP   0x0000700000000000ULL
/* The stack is committed up front rather than grown on demand -- there is no
 * fault handler that extends it -- so it has to be big enough for the deepest
 * program we intend to run.  BusyBox recurses through directory trees and
 * musl's printf machinery is not shy with locals; 1 MiB is 256 frames, which
 * this machine can spare far more easily than it can debug a silent overflow
 * into unmapped space. */
#define USER_STACK_SIZE  (256 * 0x1000ULL)         /* 1 MiB */
#define USER_BRK_BASE    0x0000600000000000ULL
#define USER_BRK_CEIL    0x00006F0000000000ULL     /* leave room above for stack */
#define USER_MMAP_BASE   0x0000500000000000ULL
#define USER_MMAP_CEIL   0x0000600000000000ULL     /* stops where the brk heap starts */

/* Everything a user process is allowed to point at lives below the canonical
 * lower-half boundary; anything else is either kernel memory or a bad
 * pointer, and we refuse to touch it. */
#define USER_LIMIT       0x0000800000000000ULL

/* True if [p, p+len) lies entirely inside the user half.  A null pointer or
 * a range that wraps/overruns USER_LIMIT is rejected -- this is the gate every
 * syscall that touches a user buffer goes through.  Defined in vmm.c so the
 * network ioctl path (net.c) can use it too. */
int user_ptr_ok(uint64_t p, uint64_t len);

typedef struct addrspace {
    uint64_t pml4_phys;
} addrspace_t;

/* Record the kernel's own PML4 so new address spaces can inherit its upper
 * half.  Call once, before any process is created. */
void vmm_init(void);

/* Make sure the kernel's BSS is backed by mapped, writable pages.  Some
 * bootloaders leave the BSS unmapped; call right after vmm_init(). */
void vmm_map_kernel_bss(void);

/* Create an address space whose lower half is empty.  NULL on failure. */
addrspace_t *vmm_create(void);

/* Free every lower-half frame and page table, then the address space. */
void vmm_destroy(addrspace_t *as);

/* Map one page.  Allocates intermediate tables as needed.  1 on success. */
int vmm_map(addrspace_t *as, uint64_t vaddr, uint64_t paddr, unsigned flags);

/* Back [vaddr, vaddr+size) with freshly zeroed frames.  1 on success. */
int vmm_alloc_range(addrspace_t *as, uint64_t vaddr, uint64_t size,
                    unsigned flags);

/* Physical address backing `vaddr`, or 0 if unmapped. */
uint64_t vmm_resolve(addrspace_t *as, uint64_t vaddr);

/* Map a physical MMIO region into the kernel address space with the
 * uncacheable attribute device registers require; returns its virtual base. */
uint64_t vmm_map_mmio(uint64_t phys, uint64_t size);

/* Drop the mapping for [vaddr, vaddr+size), freeing the backing frames.
 * 1 on success.  Intermediate page tables are left in place. */
int vmm_unmap(addrspace_t *as, uint64_t vaddr, uint64_t size);

/* Deep-copy the lower half of `src` into a brand new address space (fork). */
addrspace_t *vmm_clone(addrspace_t *src);

/* Load this address space into CR3. */
void vmm_switch(addrspace_t *as);

/* Go back to the bootloader's (kernel-only) page tables.  Needed before an
 * address space can be torn down by the very task that is running on it. */
void vmm_switch_kernel(void);

/* Copy into a user address space that may not be the current one. */
int vmm_copy_to_user(addrspace_t *as, uint64_t dst, const void *src, uint64_t n);

#endif
