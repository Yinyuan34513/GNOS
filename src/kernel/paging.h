/*
 * paging.h — page-table surgery on the address space Limine handed us.
 * (GPLv2)
 *
 * We deliberately do *not* build a page-table tree from scratch.  Limine
 * already leaves us with a perfectly good one (kernel in the higher half,
 * all of physical RAM direct-mapped at the HHDM offset, low memory identity
 * mapped) and rebuilding it is an excellent way to triple-fault.  All we
 * actually need for ring 3 is to flip the U/S bit on the handful of leaf
 * entries that cover the init image and its stack.
 */
#ifndef GNUCOS_PAGING_H
#define GNUCOS_PAGING_H

#include <stdint.h>

/* Make [vaddr, vaddr+size) reachable from CPL 3: writable, executable and
 * with U/S set at every level of the walk.  Large pages covering the range
 * are split so that only the requested region becomes user-visible. */
void paging_make_user(uint64_t vaddr, uint64_t size);

#endif
