/* stdbool.h — freestanding bool for the kernel (C11 _Bool). (GPLv2)
 * The kernel compiles with -nostdinc, so the compiler's <stdbool.h> is not
 * on the include path; provide the three macros over C11's builtin _Bool. */
#ifndef _STDBOOL_H
#define _STDBOOL_H

#define bool  _Bool
#define true  1
#define false 0
#define __bool_true_false_are_defined 1

#endif /* _STDBOOL_H */
