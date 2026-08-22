// ================================================================
// theme.h — APinux OS 淡蓝主题引擎
// 功能：色彩方案、字体渲染、双缓冲、窗口装饰
// ================================================================
#pragma once
#include <cstdint>

// 淡蓝主题配色
namespace Theme {
    constexpr uint32_t PRIMARY_BG     = 0xFFF0F8FF;  // 极浅蓝背景
    constexpr uint32_t TITLE_BAR      = 0xFF4A90D9;  // 深蓝标题栏
    constexpr uint32_t BUTTON_BG      = 0xFF7EB8E0;  // 按钮蓝色
    constexpr uint32_t BUTTON_HOVER   = 0xFF5A9BD5;  // 悬停
    constexpr uint32_t TEXT_PRIMARY   = 0xFF1A2B3C;  // 主文字色
    constexpr uint32_t TEXT_WHITE     = 0xFFFFFFFF;  // 白色文字
    constexpr uint32_t BORDER_COLOR   = 0xFF9DC8E8;  // 边框色
    constexpr uint32_t DESKTOP_BG     = 0xFFDCE9F5;  // 桌面背景
    constexpr uint32_t TASKBAR_BG     = 0xFF3A7CC0;  // 任务栏蓝
    constexpr uint32_t WINDOW_SHADOW  = 0x80000000;  // 半透明阴影
}

// 字体渲染优化（内置位图字体）
extern const uint8_t font_bitmap[256][16];  // 8x16 点阵字体
void draw_text_opt(int x, int y, const char* text, uint32_t color, int scale = 1);
void draw_char_opt(int x, int y, char c, uint32_t color, int scale = 1);