// ================================================================
// bluetooth_app.cpp — 蓝牙管理面板实现
// ================================================================
#include "bluetooth_app.h"
#include <cstring>
#include <cstdio>

BluetoothApp::BluetoothApp(Desktop* d, BLUDriver* b) : desktop(d), blu(b) {
    window_id = d->create_window("蓝牙管理", 400, 300);
    device_count = 0;
    const char* placeholder[] = {"(点击扫描)"};
    device_list = new ListBox(20, 40, 360, 150, placeholder, 1, nullptr);
    d->add_widget(window_id, device_list);
    scan_btn = new Button(20, 200, 100, 30, "扫描", [this]() { scan_devices(); });
    d->add_widget(window_id, scan_btn);
    pair_btn = new Button(140, 200, 100, 30, "配对", [this]() { pair(); });
    d->add_widget(window_id, pair_btn);
    connect_btn = new Button(260, 200, 100, 30, "连接", [this]() { connect(); });
    d->add_widget(window_id, connect_btn);
    status_label = new Label(20, 250, "就绪", Theme::TEXT_PRIMARY);
    d->add_widget(window_id, status_label);
}

void BluetoothApp::scan_devices() {
    blu->discover(device_addrs, &device_count);
    const char* items[32];
    char name_buf[32][32];
    for (int i=0; i<device_count; ++i) {
        snprintf(name_buf[i], 32, "%02X:%02X:%02X:%02X:%02X:%02X",
                 device_addrs[i][0], device_addrs[i][1], device_addrs[i][2],
                 device_addrs[i][3], device_addrs[i][4], device_addrs[i][5]);
        items[i] = name_buf[i];
    }
    delete device_list;
    device_list = new ListBox(20, 40, 360, 150, items, device_count, nullptr);
    desktop->add_widget(window_id, device_list);
}

void BluetoothApp::pair() {
    int idx = device_list->selected;
    if (idx >=0 && idx < device_count) {
        bool ok = blu->pair(device_addrs[idx]);
        status_label->text = ok ? "配对成功" : "配对失败";
    }
}

void BluetoothApp::connect() {
    int idx = device_list->selected;
    if (idx >=0 && idx < device_count) {
        bool ok = blu->connect(device_addrs[idx]);
        status_label->text = ok ? "连接成功" : "连接失败";
    }
}