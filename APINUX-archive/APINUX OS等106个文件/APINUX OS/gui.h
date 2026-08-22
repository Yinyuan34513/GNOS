// ================================================================
// gui.h — APinux OS 图形化桌面 (Framebuffer 直接渲染)
// 功能：桌面背景、任务栏、窗口管理、开机动画
// ================================================================
#pragma once
#include "kernel.h"
#include <cstdint>

// 屏幕参数 (可配置)
constexpr int SCREEN_WIDTH  = 1024;
constexpr int SCREEN_HEIGHT = 600;
constexpr int TASKBAR_HEIGHT = 32;

// 颜色格式：32-bit ARGB
using Color = uint32_t;

class Framebuffer {
    Color* fb;
    size_t fb_size;
public:
    bool init(void* phys_addr, int width, int height);
    void set_pixel(int x, int y, Color c);
    void fill_rect(int x, int y, int w, int h, Color c);
    void draw_text(int x, int y, const char* text, Color fg, Color bg);
    void clear(Color bg);
    void present();  // 双缓冲交换
};

// ---------- 开机动画 ----------
class BootAnimation {
    Framebuffer* fb;
public:
    void play(Framebuffer* framebuffer);
private:
    void draw_logo(int frame);
};

// ---------- 桌面 ----------
class Desktop {
    Framebuffer* fb;
    Color wallpaper = 0xFF1A1A2E;
public:
    void render(Framebuffer* framebuffer);
    void draw_taskbar();
    void draw_icons();
};

// ---------- 窗口管理 ----------
struct Window {
    int x, y, w, h;
    char title[64];
    Color* buffer;    // 窗口内容
    bool visible;
    Window* next;
};

class WindowManager {
    Window* wins = nullptr;
    Framebuffer* fb;
public:
    void attach(Framebuffer* framebuffer) { fb = framebuffer; }
    Window* create_window(int x, int y, int w, int h, const char* title);
    void destroy_window(Window* win);
    void render_all();
    void bring_to_front(Window* win);
};

// ---------- 任务栏 ----------
class Taskbar {
    Framebuffer* fb;
    WindowManager* wm;
public:
    void init(Framebuffer* framebuffer, WindowManager* manager);
    void render();
    void handle_click(int x, int y);
};

// ---------- 桌面环境主循环 ----------
class DesktopEnvironment {
    Framebuffer fb;
    Desktop desktop;
    WindowManager wm;
    Taskbar taskbar;
    BootAnimation boot_anim;
public:
    bool start(uint32_t framebuffer_addr);
    void run();     // 事件循环
};