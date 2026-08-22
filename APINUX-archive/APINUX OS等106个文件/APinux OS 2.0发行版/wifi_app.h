// ================================================================
// wifi_app.h — WiFi 图形化配置应用
// 功能：扫描网络、连接、断开、信号强度显示
// ================================================================
#pragma once
#include "widgets.h"
#include "desktop.h"
#include "drivers.h"

class WiFiApp {
public:
    WiFiApp(Desktop* desktop, WLANDriver* wlan);
    void show();
private:
    Desktop* desktop;
    WLANDriver* wlan;
    int window_id;
    ListBox* network_list;
    TextBox* password_box;
    Button* connect_btn;
    Button* scan_btn;
    Label* status_label;
    Label* signal_label;
    char ssids[32][32];
    int ssid_count;

    void scan_networks();
    void connect();
};