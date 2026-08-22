// ================================================================
// kbd.h — APinux OS PS/2 键盘驱动（轮询，QEMU GTK 窗口输入）
// ================================================================
#ifndef APINUX_KBD_H
#define APINUX_KBD_H

#include <stdint.h>

void kbd_init(void);
int  kbd_ready(void);             // 1 = 有可读字符
char kbd_readchar(void);          // 阻塞读一个字符（带 Shift/Caps/Ctrl 翻译）

#endif