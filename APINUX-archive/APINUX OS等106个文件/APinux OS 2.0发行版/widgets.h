// ================================================================
// widgets.h — GUI 控件库（按钮、输入框、进度条、列表）
// ================================================================
#pragma once
#include "theme.h"
#include <functional>
#include <cstring>

struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const { return px>=x && px<x+w && py>=y && py<y+h; }
};

class Widget {
public:
    Rect bounds;
    virtual void draw() = 0;
    virtual void handle_mouse(int x, int y, bool click) = 0;
    virtual ~Widget() = default;
};

// 按钮
class Button : public Widget {
    const char* text;
    std::function<void()> onClick;
    bool hovered = false;
public:
    Button(int x, int y, int w, int h, const char* t, std::function<void()> cb)
        : text(t), onClick(cb) { bounds = {x,y,w,h}; }
    void draw() override {
        uint32_t bg = hovered ? Theme::BUTTON_HOVER : Theme::BUTTON_BG;
        fill_rect(bounds.x, bounds.y, bounds.w, bounds.h, bg);
        draw_rect(bounds.x, bounds.y, bounds.w, bounds.h, Theme::BORDER_COLOR);
        int tw = strlen(text) * 8;
        int tx = bounds.x + (bounds.w - tw) / 2;
        int ty = bounds.y + (bounds.h - 16) / 2;
        draw_text_opt(tx, ty, text, Theme::TEXT_WHITE);
    }
    void handle_mouse(int x, int y, bool click) override {
        hovered = bounds.contains(x, y);
        if (hovered && click && onClick) onClick();
    }
};

// 文本标签
class Label : public Widget {
    const char* text;
    uint32_t color;
public:
    Label(int x, int y, const char* t, uint32_t c = Theme::TEXT_PRIMARY) : text(t), color(c) { bounds = {x,y,0,16}; }
    void draw() override {
        draw_text_opt(bounds.x, bounds.y, text, color);
    }
    void handle_mouse(int, int, bool) override {}
};

// 输入框
class TextBox : public Widget {
    char buffer[128];
    size_t len = 0;
    bool focused = false;
public:
    TextBox(int x, int y, int w) {
        bounds = {x, y, w, 20};
        buffer[0] = '\0';
    }
    void draw() override {
        fill_rect(bounds.x, bounds.y, bounds.w, bounds.h, 0xFFFFFFFF);
        draw_rect(bounds.x, bounds.y, bounds.w, bounds.h, focused ? Theme::TITLE_BAR : Theme::BORDER_COLOR);
        draw_text_opt(bounds.x+4, bounds.y+2, buffer, Theme::TEXT_PRIMARY);
        if (focused) draw_text_opt(bounds.x+4+len*8, bounds.y+2, "|", Theme::TEXT_PRIMARY);
    }
    void handle_mouse(int x, int y, bool click) override {
        focused = bounds.contains(x, y);
    }
    void handle_key(char c) {
        if (len < 127) {
            buffer[len++] = c;
            buffer[len] = '\0';
        }
    }
    void handle_backspace() { if (len>0) len--; buffer[len]='\0'; }
    const char* get_text() const { return buffer; }
};

// 进度条
class ProgressBar : public Widget {
    float progress; // 0.0~1.0
public:
    ProgressBar(int x, int y, int w, float p=0.0f) : progress(p) { bounds = {x,y,w,12}; }
    void draw() override {
        fill_rect(bounds.x, bounds.y, bounds.w, bounds.h, 0xFFCCCCCC);
        int fill_w = (int)(bounds.w * progress);
        if (fill_w > 0) fill_rect(bounds.x, bounds.y, fill_w, bounds.h, Theme::TITLE_BAR);
        draw_rect(bounds.x, bounds.y, bounds.w, bounds.h, Theme::BORDER_COLOR);
    }
    void set_progress(float p) { progress = p; }
    void handle_mouse(int, int, bool) override {}
};

// 简单列表
class ListBox : public Widget {
    const char** items;
    int count;
    int selected = -1;
    std::function<void(int)> onSelect;
public:
    ListBox(int x, int y, int w, int h, const char** its, int cnt, std::function<void(int)> cb)
        : items(its), count(cnt), onSelect(cb) { bounds = {x,y,w,h}; }
    void draw() override {
        fill_rect(bounds.x, bounds.y, bounds.w, bounds.h, 0xFFFFFFFF);
        draw_rect(bounds.x, bounds.y, bounds.w, bounds.h, Theme::BORDER_COLOR);
        for (int i=0; i<count; ++i) {
            uint32_t bg = (i==selected) ? Theme::BUTTON_HOVER : 0xFFFFFFFF;
            fill_rect(bounds.x+2, bounds.y+2+i*20, bounds.w-4, 18, bg);
            draw_text_opt(bounds.x+4, bounds.y+2+i*20+2, items[i], Theme::TEXT_PRIMARY);
        }
    }
    void handle_mouse(int x, int y, bool click) override {
        if (click && bounds.contains(x, y)) {
            int idx = (y - bounds.y) / 20;
            if (idx >=0 && idx < count) {
                selected = idx;
                if (onSelect) onSelect(idx);
            }
        }
    }
};