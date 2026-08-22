// ================================================================
// userlib.h — APinux OS 用户空间基础库 (C++)
// 功能：标准输入输出、文件操作、内存分配
// 构建修复版：
//   1. struct FILE 定义移到 fopen/fread 声明之前
//      （原文件把定义放在文件末尾，导致 'FILE' does not name a type）
//   2. malloc/free/strlen/strcpy/strcmp/strncmp 的定义移出本头——
//      原为普通函数定义：多个 TU 包含时报 multiple definition；
//      改 inline 后又因未 ODR-used 的 TU 拿不到符号而报 undefined。
//      现在这些函数由 libc_support.cpp 提供单一全局定义，
//      本头仅保留声明（extern "C"，与 <cstring>/<cstdlib> 一致）。
// ================================================================
#pragma once
#include "kernel.h"
#include <cstdarg>
#include <cstdlib>

// ---------- 文件描述符 ----------
struct FILE { int fd; void* buf; size_t pos; size_t size; };

// ---------- 简化 libc（实现见 userlib.cpp / libc_support.cpp） ----------
extern "C" void* malloc(size_t size);
extern "C" void free(void* ptr);
extern "C" size_t strlen(const char* s);
extern "C" char* strcpy(char* dst, const char* src);
extern "C" int strcmp(const char* a, const char* b);
extern "C" int strncmp(const char* a, const char* b, size_t n);

int printf(const char* fmt, ...);

FILE* fopen(const char* path, const char* mode);
size_t fread(void* buf, size_t size, size_t nmemb, FILE* f);
size_t fwrite(const void* buf, size_t size, size_t nmemb, FILE* f);
int fclose(FILE* f);
