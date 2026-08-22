// ================================================================
// kbd.cpp — APinux OS PS/2 键盘驱动（轮询实现）
//   端口：0x60 数据 / 0x64 状态与命令
//   扫描码：Set 1（QEMU 默认），带 Shift/CapsLock/Ctrl 状态翻译，
//   忽略 0xE0 扩展键（方向键等）与全部 break 码。
// ================================================================
#include "kbd.h"

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline int kbd_output_full(void) { return inb(0x64) & 1; }
static inline int kbd_input_empty(void) { return (inb(0x64) & 2) == 0; }

// ---- Set 1 扫描码 → 字符表（make 码，break 码 = 0x80 + make） ----
// 0 表示该键不产生字符（修饰键 / 功能键 / 扩展键）
static const char sc_normal[128] = {
    /* 0x00 */ 0,    0x1B,  '1',  '2',  '3',  '4',  '5',  '6',
    /* 0x08 */ '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',
    /* 0x10 */ 'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',
    /* 0x18 */ 'o',  'p',  '[',  ']',  '\r', 0,    'a',  's',
    /* 0x20 */ 'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',
    /* 0x28 */ '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',
    /* 0x30 */ 'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',
    /* 0x38 */ 0,    ' ',  0,    0,    0,    0,    0,    0,
    /* 0x40 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x48 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x50 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x58 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x60 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x68 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x70 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x78 */ 0,    0,    0,    0,    0,    0,    0,    0,
};

// 数字行与符号区的 Shift 上档字符（与 sc_normal 同位）
static const char sc_shift[128] = {
    /* 0x00 */ 0,    0x1B,  '!',  '@',  '#',  '$',  '%',  '^',
    /* 0x08 */ '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',
    /* 0x10 */ 'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',
    /* 0x18 */ 'O',  'P',  '{',  '}',  '\r', 0,    'A',  'S',
    /* 0x20 */ 'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',
    /* 0x28 */ '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',
    /* 0x30 */ 'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',
    /* 0x38 */ 0,    ' ',  0,    0,    0,    0,    0,    0,
    /* 0x40 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x48 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x50 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x58 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x60 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x68 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x70 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x78 */ 0,    0,    0,    0,    0,    0,    0,    0,
};

// ---- 状态 ----
static bool g_shift = false;
static bool g_caps  = false;
static bool g_ctrl  = false;
static bool g_e0    = false;   // 0xE0 前缀（扩展键，忽略）

void kbd_init(void) {
    // 清空输出缓冲中的残留字节
    for (int i = 0; i < 16 && kbd_output_full(); i++)
        (void)inb(0x60);
    // 启用键盘（0xAE 到 0x64），等待输入缓冲空
    for (int i = 0; i < 10000 && !kbd_input_empty(); i++)
        asm volatile("pause");
    outb(0x64, 0xAE);
    g_shift = g_caps = g_ctrl = g_e0 = false;
}

int kbd_ready(void) {
    return kbd_output_full() ? 1 : 0;
}

static char translate(uint8_t sc) {
    if (sc == 0xE0) {          // 扩展前缀：吞掉后续一个字节
        g_e0 = true;
        return 0;
    }
    if (g_e0) {                // 扩展键（方向键等）：直接忽略
        g_e0 = false;
        return 0;
    }
    if (sc & 0x80) {           // break 码
        uint8_t mk = sc & 0x7F;
        if (mk == 0x2A || mk == 0x36) g_shift = false;
        if (mk == 0x1D) g_ctrl = false;
        return 0;
    }
    switch (sc) {
        case 0x2A: case 0x36: g_shift = true; return 0;
        case 0x1D: g_ctrl = true; return 0;
        case 0x3A: g_caps = !g_caps; return 0;
        default: break;
    }
    char base = sc_normal[sc & 0x7F];
    if (!base)
        return 0;
    if (g_ctrl) {
        // Ctrl+字母 → 控制字符（Ctrl+C = 0x03 等）
        if (base >= 'a' && base <= 'z')
            return (char)(base - 'a' + 1);
        return 0;
    }
    bool upper = g_shift != g_caps;
    if (upper && sc_shift[sc & 0x7F])
        return sc_shift[sc & 0x7F];
    return base;
}

char kbd_readchar(void) {
    for (;;) {
        while (!kbd_output_full())
            asm volatile("pause");
        uint8_t sc = inb(0x60);
        char c = translate(sc);
        if (c)
            return c;
    }
}