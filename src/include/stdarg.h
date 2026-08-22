/* stdarg.h — freestanding varargs for the kernel (GCC builtins). (GPLv2)
 * The kernel compiles with -nostdinc, so the compiler's <stdarg.h> is not
 * on the include path; map the C89 varargs interface onto GCC's builtins
 * (__builtin_va_list and friends, which exist even under -ffreestanding). */
#ifndef _STDARG_H
#define _STDARG_H

typedef __builtin_va_list va_list;

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_copy(dst, src)  __builtin_va_copy(dst, src)

#endif /* _STDARG_H */
