#ifndef _STDDEF_H
#define _STDDEF_H

typedef unsigned int size_t;
typedef int          ptrdiff_t;

#define NULL ((void *)0)

/* container_of() and the DRM core's mode objects need offsetof.  GCC's
 * __builtin_offsetof is available even under -ffreestanding. */
#define offsetof(type, member) __builtin_offsetof(type, member)

#endif