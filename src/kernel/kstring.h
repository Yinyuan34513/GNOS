/*
 * kstring.h — the handful of libc string/memory routines a freestanding
 * kernel still needs.  GCC is allowed to emit calls to memcpy, memmove,
 * memset and memcmp even with -ffreestanding, so these must exist. (GPLv2)
 */
#ifndef GNUCOS_KSTRING_H
#define GNUCOS_KSTRING_H

#include <stddef.h>
#include <stdint.h>

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strncpy(char *dst, const char *src, size_t n);

#endif
