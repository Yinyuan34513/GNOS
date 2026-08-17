/*
 * heap.h — the kernel heap: a general-purpose kmalloc/kfree. (GPLv2)
 *
 * Backed by one contiguous run of physical frames from the PMM, viewed
 * through the HHDM direct map so it is a single linear virtual region.  The
 * allocator is an explicit doubly-linked free list with boundary tags, which
 * is what makes kfree() able to coalesce neighbours in O(1).  Blocks are
 * 16-byte aligned; every block carries a header and a footer (the footer
 * holds the size so a freed block can find its predecessor without a second
 * list).  The heap is sized once at boot -- it does not grow.
 */
#ifndef GNUCOS_HEAP_H
#define GNUCOS_HEAP_H

#include <stddef.h>

/* Bring the heap up: carve a contiguous region from the PMM.  Called once,
 * after pmm_init() and after the HHDM is known. */
void kheap_init(void);

/* Allocate `size` bytes, or NULL if the heap is exhausted.  The result is
 * 16-byte aligned and may be arbitrary data (use kzalloc for zeroed memory). */
void *kmalloc(size_t size);

/* Release a block returned by kmalloc/kzalloc/kcalloc.  NULL is ignored. */
void kfree(void *p);

/* Allocate zeroed memory (kmalloc + memset(0)). */
void *kzalloc(size_t size);

/* Allocate `n` elements of `size` bytes, zeroed. */
void *kcalloc(size_t n, size_t size);

/* Non-fatal self check: allocate/free a handful of blocks and verify the
 * bookkeeping (no overlaps, coalescing restores a single free span).  Prints
 * a one-line result to the debug console. */
void kheap_self_test(void);

#endif
