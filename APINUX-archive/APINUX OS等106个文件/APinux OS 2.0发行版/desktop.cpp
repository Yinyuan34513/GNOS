// ================================================================
// desktop.cpp — APinux OS 桌面环境实现
// ================================================================
#include "desktop.h"
#include <cstring>

Desktop::Desktop() {
    // 创建初始桌面窗口（壁纸）
    create_window("Desktop", screen_w, screen_h);
}

int Desktop::create_window(const char* title, int w, int h) {
    if (window_count >= MAX_WINDOWS) return -1;
    int id = window_count++;
    AppWindow& win = windows[id];
    win.id = id;
    win.title = title;
    win.bounds = {50 + id*20, 50 + id*20, w, h};
    win.visible = true;
    win.minimized = false;
    return id;
}

void Desktop::close_window(int id) {
    if (id >=0 && id < window_count) {
        windows[id].visible = false;
        windows[id].widgets.clear();
    }
}

void Desktop::set_active(int id) {
    if (id >=0 && id < window_count && windows[id].visible) {
        active_window = id;
    }
}

void Desktop::add_widget(int win_id, Widget* w) {
    if (win_id >=0 && win_id < window_count) {
        windows[win_id].widgets.push_back(w);
    }
}

void Desktop::draw() {
    // 清除屏幕
    fill_rect(0, 0, screen_w, screen_h, Theme::DESKTOP_BG);

    // 绘制所有可见窗口（从后往前，活动窗口最后画）
    for (int i = 0; i < window_count; ++i) {
        if (i == active_window) continue;
        AppWindow& win = windows[i];
        if (!win.visible || win.minimized) continue;
        // 窗口背景
        fill_rect(win.bounds.x, win.bounds.y, win.bounds.w, win.bounds.h, Theme::PRIMARY_BG);
        // 标题栏
        fill_rect(win.bounds.x, win.bounds.y, win.bounds.w, 24, Theme::TITLE_BAR);
        draw_text_opt(win.bounds.x+8, win.bounds.y+4, win.title, Theme::TEXT_WHITE);
        // 窗口边框
        draw_rect(win.bounds.x, win.bounds.y, win.bounds.w, win.bounds.h, Theme::BORDER_COLOR);
        // 控件
        for (auto* w : win.widgets) w->draw();
    }
    // 活动窗口最后画（确保在最上层）
    if (active_window >=0) {
        AppWindow& aw = windows[active_window];
        if (aw.visible && !aw.minimized) {
            fill_rect(aw.bounds.x, aw.bounds.y, aw.bounds.w, aw.bounds.h, Theme::PRIMARY_BG);
            fill_rect(aw.bounds.x, aw.bounds.y, aw.bounds.w, 24, Theme::TITLE_BAR);
            draw_text_opt(aw.bounds.x+8, aw.bounds.y+4, aw.title, Theme::TEXT_WHITE);
            draw_rect(aw.bounds.x, aw.bounds.y, aw.bounds.w, aw.bounds.h, Theme::BORDER_COLOR);
            for (auto* w : aw.widgets) w->draw();
        }
    }
    // 任务栏
    fill_rect(0, screen_h - taskbar_height, screen_w, taskbar_height, Theme::TASKBAR_BG);
    draw_text_opt(10, screen_h - taskbar_height + 10, "APinux OS", Theme::TEXT_WHITE);
    // 开始按钮
    draw_text_opt(screen_w - 80, screen_h - taskbar_height + 10, "Start", Theme::TEXT_WHITE);
}

void Desktop::handle_mouse(int x, int y, bool click) {
    // 检测活动窗口
    for (int i = window_count-1; i >=0; --i) {
        AppWindow& win = windows[i];
        if (!win.visible || win.minimized) continue;
        if (win.bounds.contains(x, y)) {
            set_active(i);
            // 转发给窗口内的控件
            for (auto* w : win.widgets) {
                w->handle_mouse(x - win.bounds.x, y - win.bounds.y - 24, click);
            }
            return;
        }
    }
    // 点击任务栏
    if (y > screen_h - taskbar_height) {
        // 这里可以处理开始菜单等
    }
}

void Desktop::handle_key(char c, uint32_t keycode) {
    // 将键盘事件转发给活动窗口的焦点控件
    if (active_window >=0) {
        for (auto* w : windows[active_window].widgets) {
            if (TextBox* tb = dynamic_cast<TextBox*>(w)) {
                if (tb->focused) {
                    if (keycode == 14) tb->handle_backspace(); // Backspace
                    else tb->handle_key(c);
                }
            }
        }
    }
}

void Desktop::run() {
    while (true) {
        draw();
        // 轮询鼠标和键盘（实际需要中断或事件机制）
        // 简化：此处由外部事件循环调用
    }
}