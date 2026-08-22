// ================================================================
// wifi_app.cpp — WiFi 管理面板实现
// ================================================================
#include "wifi_app.h"
#include <cstring>

WiFiApp::WiFiApp(Desktop* d, WLANDriver* w) : desktop(d), wlan(w) {
    window_id = d->create_window("WiFi 管理", 400, 350);
    ssid_count = 0;
    const char* placeholder[] = {"(点击扫描)"};
    network_list = new ListBox(20, 40, 360, 180, placeholder, 1, [this](int idx) {
        // 选中网络时自动填入 SSID 到密码框上方
    });
    d->add_widget(window_id, network_list);
    password_box = new TextBox(20, 230, 360);
    d->add_widget(window_id, password_box);
    connect_btn = new Button(20, 270, 100, 30, "连接", [this]() { connect(); });
    d->add_widget(window_id, connect_btn);
    scan_btn = new Button(140, 270, 100, 30, "扫描", [this]() { scan_networks(); });
    d->add_widget(window_id, scan_btn);
    status_label = new Label(20, 310, "就绪", Theme::TEXT_PRIMARY);
    d->add_widget(window_id, status_label);
    signal_label = new Label(250, 310, "", Theme::TEXT_PRIMARY);
    d->add_widget(window_id, signal_label);
}

void WiFiApp::scan_networks() {
    wlan->scan(ssids, &ssid_count);
    const char* items[32];
    for (int i=0; i<ssid_count; ++i) items[i] = ssids[i];
    // 更新列表
    delete network_list;
    network_list = new ListBox(20, 40, 360, 180, items, ssid_count, [this](int idx) {});
    desktop->add_widget(window_id, network_list);
}

void WiFiApp::connect() {
    const char* pw = password_box->get_text();
    int idx = network_list->selected;
    if (idx >=0 && idx < ssid_count) {
        bool ok = wlan->connect(ssids[idx], pw);
        status_label->text = ok ? "连接成功" : "连接失败";
    }
}