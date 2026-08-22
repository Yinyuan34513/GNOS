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
#if defined(__x86_64__)
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
#endif

void serial_write(const char* s) {
#if defined(__x86_64__)
    // x86_64: COM1 (0x3F8)，配合 QEMU -serial stdio
    static int com1_ready = 0;
    if (!com1_ready) {
        outb(0x3F9, 0x00);   // IER: 关中断
        outb(0x3FB, 0x03);   // LCR: 8N1
        outb(0x3FA, 0x01);   // FCR: 开 FIFO
        com1_ready = 1;
    }
    while (*s) {
        while ((inb(0x3F8 + 5) & 0x20) == 0) { }  // 等 THR 空
        outb(0x3F8, (uint8_t)*s++);
    }
#else
    // ARM: UART MMIO (地址由设备树决定)
    volatile uint32_t* uart = (uint32_t*)0x10000000;
    while (*s) {
        *uart = (uint32_t)*s++;
        for (volatile int i = 0; i < 1000; ++i);
    }
#endif
}

// ================================================================
// 构建修复：GUI 层需要的 libc 补全（-nostdlib 无 libc）
//   bluetooth_app/file_manager 使用 snprintf 与 strrchr
//   注：strrchr 经 <cstring> 提供
//   glibc 的 C++ 重载声明，重复定义报 "ambiguating new declaration"
// ================================================================
extern "C" int snprintf(char* str, size_t size, const char* fmt, ...) {
    if (!str || size == 0) return 0;
    va_list args;
    va_start(args, fmt);
    size_t pos = 0;
    while (*fmt && pos + 1 < size) {
        if (*fmt != '%') { str[pos++] = *fmt++; continue; }
        ++fmt;
        // flags: 仅支持 '0' + 宽度（如 %02X）
        int width = 0;
        bool zero = false;
        while (*fmt == '0' || (*fmt >= '1' && *fmt <= '9')) {
            if (*fmt == '0' && width == 0) zero = true;
            else width = width * 10 + (*fmt - '0');
            ++fmt;
        }
        char tmp[32];
        int ti = 0;
        switch (*fmt) {
            case 's': {
                const char* s = va_arg(args, const char*);
                if (!s) s = "(null)";
                while (*s && ti < 31) tmp[ti++] = *s++;
                break;
            }
            case 'c':
                tmp[ti++] = (char)va_arg(args, int);
                break;
            case 'd': case 'i': {
                int v = va_arg(args, int);
                unsigned int u = v < 0 ? (unsigned int)(-v) : (unsigned int)v;
                char rev[16]; int ri = 0;
                if (u == 0) rev[ri++] = '0';
                while (u > 0 && ri < 16) { rev[ri++] = '0' + (u % 10); u /= 10; }
                if (v < 0) rev[ri++] = '-';
                while (ri > 0 && ti < 31) tmp[ti++] = rev[--ri];
                break;
            }
            case 'u': {
                unsigned int u = va_arg(args, unsigned int);
                char rev[16]; int ri = 0;
                if (u == 0) rev[ri++] = '0';
                while (u > 0 && ri < 16) { rev[ri++] = '0' + (u % 10); u /= 10; }
                while (ri > 0 && ti < 31) tmp[ti++] = rev[--ri];
                break;
            }
            case 'x': case 'X': {
                unsigned int u = va_arg(args, unsigned int);
                char rev[16]; int ri = 0;
                char a = (*fmt == 'X') ? 'A' : 'a';
                if (u == 0) rev[ri++] = '0';
                while (u > 0 && ri < 16) {
                    int d = u & 0xF;
                    rev[ri++] = d < 10 ? '0' + d : a + d - 10;
                    u >>= 4;
                }
                while (ri > 0 && ti < 31) tmp[ti++] = rev[--ri];
                break;
            }
            default:
                tmp[ti++] = '%';
                if (*fmt) tmp[ti++] = *fmt;
                break;
        }
        if (*fmt) ++fmt;
        // 补齐到 width（'0' 或空格，插在字段前）
        while (ti < width && ti < 31) {
            for (int i = ti; i > 0; --i) tmp[i] = tmp[i - 1];
            tmp[0] = zero ? '0' : ' ';
            ++ti;
        }
        for (int i = 0; i < ti && pos + 1 < size; ++i) str[pos++] = tmp[i];
    }
    str[pos] = '\0';
    va_end(args);
    return (int)pos;
}