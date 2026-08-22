// ================================================================
// console.h — APinux OS 控制台（VGA 文本 / 帧缓冲双通道）
// 优先使用 Multiboot 帧缓冲（8x16 字体绘制），无帧缓冲时回退
// VGA 文本模式（0xB8000）。
// ================================================================
#ifndef APINUX_CONSOLE_H
#define APINUX_CONSOLE_H

#include <stdint.h>

void console_init(uint32_t magic, uint32_t info);
void console_clear(void);
void console_putchar(char c);
void console_write(const char* s);

#endif