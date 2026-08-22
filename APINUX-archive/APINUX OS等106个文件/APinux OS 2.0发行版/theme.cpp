// ================================================================
// theme.cpp — 主题引擎实现（字体渲染、双缓冲辅助）
// ================================================================
#include "theme.h"
#include <cstring>

// 8x16 点阵字体位图（ASCII 可见字符）
const uint8_t font_bitmap[256][16] = {
    // 省略具体位图数据，实际应包含完整 ASCII 字符集
    // 此处以示例说明结构
    ['A'] = {0x00,0x18,0x24,0x42,0x42,0x7E,0x42,0x42,0x42,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['B'] = {0x00,0x7C,0x42,0x42,0x7C,0x42,0x42,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // ... 其他字符
};

extern uint32_t* framebuffer;   // 显存线性帧缓冲
extern int screen_w, screen_h;  // 屏幕分辨率

void draw_char_opt(int x, int y, char c, uint32_t color, int scale) {
    if (c < 0 || c > 255) return;
    const uint8_t* glyph = font_bitmap[(unsigned char)c];
    for (int row = 0; row < 16; ++row) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; ++col) {
            if (bits & (0x80 >> col)) {
                for (int dy = 0; dy < scale; ++dy)
                    for (int dx = 0; dx < scale; ++dx) {
                        int px = x + col * scale + dx;
                        int py = y + row * scale + dy;
                        if (px >= 0 && px < screen_w && py >= 0 && py < screen_h)
                            framebuffer[py * screen_w + px] = color;
                    }
            }
        }
    }
}

void draw_text_opt(int x, int y, const char* text, uint32_t color, int scale) {
    int cur_x = x;
    for (size_t i = 0; text[i]; ++i) {
        if (text[i] == '\n') {
            cur_x = x;
            y += 16 * scale;
            continue;
        }
        draw_char_opt(cur_x, y, text[i], color, scale);
        cur_x += 8 * scale;
    }
}