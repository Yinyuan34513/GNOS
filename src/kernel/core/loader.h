/*
 * loader.h — ELF64 / a.out image loader. (GPLv2)
 *
 * Loads an executable into a *specific* address space rather than the current
 * one, which is what execve() needs: the new image has to be built before the
 * old one is thrown away, and the caller is still running on the old mappings
 * while that happens.  Segment contents therefore travel through the HHDM
 * direct map (vmm_copy_to_user) instead of being written at their final
 * virtual addresses.
 */
#ifndef GNUCOS_LOADER_H
#define GNUCOS_LOADER_H

#include <stdint.h>
#include "vmm.h"

/* Size of one ELF64 program header entry, i.e. what AT_PHENT has to report.
 * Linux hardcodes this the same way -- NEW_AUX_ENT(AT_PHENT, sizeof(struct
 * elf_phdr)) -- rather than echoing the file's e_phentsize back at userspace. */
#define ELF64_PHDR_SIZE 56

/*
 * Return value:
 *    1  loaded as ELF
 *    2  loaded as a.out
 *    0  unknown format
 *   -1  ELF magic present but image malformed
 *   -2  a.out magic present but image malformed
 *
 * On success *phdr holds the runtime address of the program header table and
 * *phnum the number of entries, or 0/0 if the table is not covered by any
 * PT_LOAD segment (a.out always reports 0/0).  Callers must treat 0 as "no
 * auxv entry to publish" rather than as a valid address: musl walks the table
 * blindly once AT_PHNUM says it exists.
 */
int load_executable(addrspace_t *as, const uint8_t *img, uint32_t size,
                    uint64_t *entry, uint64_t *phdr, uint16_t *phnum);

#endif
