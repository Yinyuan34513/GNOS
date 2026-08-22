// ================================================================
// settings_app.h — 系统设置面板
// 功能：分辨率调整、主题切换、网络配置、用户管理
// ================================================================
#pragma once
#include "widgets.h"
#include "desktop.h"
#include "config.h"

class SettingsApp {
public:
    SettingsApp(Desktop* desktop, ConfigManager* cfg);
    void show();
private:
    Desktop* desktop;
    ConfigManager* config;
    int window_id;
    ListBox* resolution_list;
    Button* apply_btn;
    TextBox* hostname_box;
    Button* save_btn;
    Label* status_label;
    void apply_resolution();
    void save_config();
};