// ================================================================
// userlib.h — APinux OS 用户空间基础库 (C++)
// 功能：标准输入输出、文件操作、内存分配
// ================================================================
#pragma once
#include "kernel.h"

// ---------- 简化 libc ----------
void* malloc(size_t size) {
    return (void*)syscall(SYS_ALLOC, size, 0, 0);
}
void free(void* ptr) {
    syscall(SYS_FREE, (uint64_t)ptr, 0, 0);
}

int printf(const char* fmt, ...);

FILE* fopen(const char* path, const char* mode);
size_t fread(void* buf, size_t size, size_t nmemb, FILE* f);
size_t fwrite(const void* buf, size_t size, size_t nmemb, FILE* f);
int fclose(FILE* f);

// 字符串操作
size_t strlen(const char* s) {
    size_t n = 0;
    while (*s++) ++n;
    return n;
}
char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++));
    return dst;
}
int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a - *b;
}
int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i] || a[i] == 0) return a[i] - b[i];
    }
    return 0;
}

// ---------- 文件描述符 ----------
struct FILE { int fd; void* buf; size_t pos; size_t size; };