// ================================================================
// userlib.cpp — APinux OS 用户空间基础库实现
// 功能：printf, fopen, fread, fwrite, fclose
// ================================================================
#include "userlib.h"
#include "vfs.h"

extern VirtualFileSystem g_vfs;

// printf 实现 (仅支持 %s, %d, %c, %x)
int printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    char* p = buf;
    while (*fmt) {
        if (*fmt == '%') {
            ++fmt;
            switch (*fmt) {
                case 's': {
                    const char* s = va_arg(args, const char*);
                    while (*s) *p++ = *s++;
                    break;
                }
                case 'd': {
                    int d = va_arg(args, int);
                    char tmp[16];
                    int i = 0;
                    if (d < 0) { *p++ = '-'; d = -d; }
                    do { tmp[i++] = '0' + (d % 10); d /= 10; } while (d);
                    while (i > 0) *p++ = tmp[--i];
                    break;
                }
                case 'c':
                    *p++ = (char)va_arg(args, int);
                    break;
                case 'x': {
                    unsigned int x = va_arg(args, unsigned int);
                    char tmp[16];
                    int i = 0;
                    do {
                        int v = x & 0xF;
                        tmp[i++] = v < 10 ? '0' + v : 'a' + v - 10;
                        x >>= 4;
                    } while (x);
                    while (i > 0) *p++ = tmp[--i];
                    break;
                }
                default:
                    *p++ = '%';
                    *p++ = *fmt;
                    break;
            }
        } else {
            *p++ = *fmt;
        }
        ++fmt;
    }
    *p = '\0';
    va_end(args);

    // 输出到串口 (简化：直接使用设备管理器输出)
    extern void serial_write(const char* s);
    serial_write(buf);
    return p - buf;
}

FILE* fopen(const char* path, const char* mode) {
    VNode* node = g_vfs.lookup(path);
    if (!node) return nullptr;
    FILE* f = (FILE*)malloc(sizeof(FILE));
    f->fd = node->inode;
    f->buf = node->data;
    f->pos = 0;
    f->size = node->size;
    return f;
}

size_t fread(void* buf, size_t size, size_t nmemb, FILE* f) {
    if (!f || !f->buf) return 0;
    size_t total = size * nmemb;
    if (f->pos + total > f->size) total = f->size - f->pos;
    memcpy(buf, (char*)f->buf + f->pos, total);
    f->pos += total;
    return total / size;
}

size_t fwrite(const void* buf, size_t size, size_t nmemb, FILE* f) {
    if (!f) return 0;
    size_t total = size * nmemb;
    VNode* node = g_vfs.lookup((const char*)(uintptr_t)f->fd); // 简化
    if (!node) return 0;
    g_vfs.write_file(node, buf, f->pos, total);
    f->pos += total;
    return nmemb;
}

int fclose(FILE* f) {
    if (f) free(f);
    return 0;
}

// 串口输出 (用于 printf)
void serial_write(const char* s) {
    // 直接写入UART MMIO (地址由设备树决定)
    volatile uint32_t* uart = (uint32_t*)0x10000000;
    while (*s) {
        *uart = (uint32_t)*s++;
        // 等待发送完成 (简化)
        for (volatile int i = 0; i < 1000; ++i);
    }
}