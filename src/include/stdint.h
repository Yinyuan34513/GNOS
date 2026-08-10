#ifndef _STDINT_H
#define _STDINT_H

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;
typedef long long          int64_t;
typedef unsigned long long uint64_t;

typedef uint64_t uintptr_t;
typedef int64_t  intptr_t;

typedef uint32_t uintmax_t;
typedef int32_t  intmax_t;

#define INT8_MAX   127
#define INT16_MAX  32767
#define INT32_MAX  2147483647
#define UINT32_MAX 4294967295U

#endif