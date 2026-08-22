// ================================================================
// libc_support.cpp — 构建修复：裸机 libc 全局定义
//
// GUI 层（wifi_app/file_manager/...）与 userlib 都用到
// malloc/free/strlen/strcpy/strcmp/strncmp，但：
//   - 原 userlib.h 把它们写成头文件内普通函数定义 → 多 TU 包含时
//     ld 报 multiple definition
//   - 改 inline 后又因为未 ODR-used 的 TU 不产生符号而报 undefined
// 本文件提供唯一全局定义。刻意只包含 <cstddef>/<cstdint>（不包含
// <cstring>/<cstdlib>），避免与 glibc 头里的 C++ 重载声明冲突
// （"ambiguating new declaration"，见 strrchr/strchr 的处理）。
// malloc/free 直接走内核 syscall（SYS_ALLOC=1 / SYS_FREE=2）。
// ================================================================
#include <cstddef>
#include <cstdint>

extern "C" uint64_t syscall(uint64_t num, uint64_t arg1, uint64_t arg2,
                            uint64_t arg3);

extern "C" void* malloc(size_t size) {
    return (void*)syscall(1, size, 0, 0);   // SYS_ALLOC
}
extern "C" void free(void* ptr) {
    syscall(2, (uint64_t)ptr, 0, 0);        // SYS_FREE
}

extern "C" size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}
extern "C" char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}
extern "C" int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}
extern "C" int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i] || a[i] == 0) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

extern "C" char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c)
            return (char*)s;
        ++s;
    }
    return ((char)c == 0) ? (char*)s : nullptr;
}

extern "C" char* strrchr(const char* s, int c) {
    const char* last = nullptr;
    do {
        if (*s == (char)c)
            last = s;
    } while (*s++);
    return (char*)last;
}

extern "C" char* strncpy(char* dst, const char* src, size_t n); // 定义在 kernel.cpp

extern "C" void* memset(void* dst, int c, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}
