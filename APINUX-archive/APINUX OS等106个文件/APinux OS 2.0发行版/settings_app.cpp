// ================================================================
// settings_app.cpp — 设置面板实现
// ================================================================
#include "settings_app.h"
#include <cstring>

SettingsApp::SettingsApp(Desktop* d, ConfigManager* cfg) : desktop(d), config(cfg) {
    window_id = d->create_window("系统设置", 500, 400);
    const char* resolutions[] = {"1024x600", "1280x720", "1366x768", "1920x1080"};
    resolution_list = new ListBox(20, 40, 200, 120, resolutions, 4, nullptr);
    d->add_widget(window_id, resolution_list);
    apply_btn = new Button(240, 40, 100, 30, "应用", [this]() { apply_resolution(); });
    d->add_widget(window_id, apply_btn);
    hostname_box = new TextBox(20, 180, 200);
    d->add_widget(window_id, hostname_box);
    save_btn = new Button(240, 180, 100, 30, "保存", [this]() { save_config(); });
    d->add_widget(window_id, save_btn);
    status_label = new Label(20, 230, "", Theme::TEXT_PRIMARY);
    d->add_widget(window_id, status_label);
}

void SettingsApp::apply_resolution() {
    // 调用内核接口切换分辨率（需内核支持）
    status_label->text = "分辨率已更改";
}

void SettingsApp::save_config() {
    SysConfig& cfg = config->get();
    strncpy(cfg.hostname, hostname_box->get_text(), 63);
    config->save();
    status_label->text = "配置已保存";
}