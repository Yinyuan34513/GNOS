// ================================================================
// desktop.h — APinux OS 新版桌面环境
// 功能：窗口管理、任务栏、系统托盘、开始菜单
// ================================================================
#pragma once
#include "widgets.h"
#include "theme.h"
#include <functional>
#include <vector>

struct AppWindow {
    int id;
    Rect bounds;
    const char* title;
    bool visible;
    bool minimized;
    std::vector<Widget*> widgets;
};

class Desktop {
public:
    static constexpr int MAX_WINDOWS = 32;
    AppWindow windows[MAX_WINDOWS];
    int window_count = 0;
    int active_window = -1;
    int taskbar_height = 36;
    bool start_menu_open = false;

    Desktop();
    int create_window(const char* title, int w, int h);
    void close_window(int id);
    void set_active(int id);
    void draw();
    void handle_mouse(int x, int y, bool click);
    void handle_key(char c, uint32_t keycode);
    void add_widget(int win_id, Widget* w);
    void run();
};