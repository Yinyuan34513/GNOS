/*
 * module_info.h -- shared bits for GNOS kernel modules. (GPLv2)
 *
 * A module source includes this (indirectly) to stamp its .modinfo
 * section; the loader reads "name=" and "license=" from it, exactly like
 * Linux's modinfo fields.  Every module should declare both.
 *
 * Module files compile with -fno-pic -fno-pie (they are plain ET_REL
 * objects; the loader relocates them), so no PIE machinery applies.
 */
#ifndef GNUCOS_MODULE_INFO_H
#define GNUCOS_MODULE_INFO_H

#define __MODULE_INFO(field, value)                                     \
    __asm__(".section .modinfo,\"a\",@progbits\n\t"                     \
            ".asciz \"" field "=" value "\"\n\t"                        \
            ".previous")

#define MODULE_INFO(field, value) __MODULE_INFO(field, value)

/* Every module must provide these. */
#define MODULE_NAME(name)    MODULE_INFO("name", name)
#define MODULE_LICENSE(lic)  MODULE_INFO("license", lic)

#endif /* GNUCOS_MODULE_INFO_H */